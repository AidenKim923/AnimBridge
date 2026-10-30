// Bake clips to the local Transform values Unity evaluates,
// then convert Humanoid/binary clips with `uab convert --baked baked.json --skeleton model.fbx`.
// Copy this file into any Editor folder of the Unity project.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using UnityEditor;
using UnityEngine;

public static class UnityAnimBridgeBaker
{
    const int BakedVersion = 1;

    [MenuItem("Tools/UnityAnimBridge/Bake Selected Clips...")]
    static void BakeSelectedMenu()
    {
        var model = Selection.objects.OfType<GameObject>().FirstOrDefault(go => PrefabUtility.IsPartOfPrefabAsset(go) || AssetDatabase.Contains(go));
        var clips = new List<AnimationClip>();
        foreach (var o in Selection.objects)
        {
            if (o is AnimationClip c && !c.name.StartsWith("__preview__")) clips.Add(c);
            else if (o is GameObject g && g != model)
                clips.AddRange(AssetDatabase.LoadAllAssetsAtPath(AssetDatabase.GetAssetPath(g)).OfType<AnimationClip>()
                    .Where(c2 => !c2.name.StartsWith("__preview__")));
        }
        if (model == null || clips.Count == 0)
        {
            EditorUtility.DisplayDialog("UnityAnimBridge", "Select the model (prefab or FBX) and one or more AnimationClips / FBX files.", "OK");
            return;
        }
        string outPath = EditorUtility.SaveFilePanel("Save baked.json", "", model.name + "_baked.json", "json");
        if (string.IsNullOrEmpty(outPath)) return;
        Bake(model, clips, 0f, outPath);
        EditorUtility.RevealInFinder(outPath);
    }

    // Unity.exe -batchmode -quit -projectPath <p> -executeMethod UnityAnimBridgeBaker.BakeFromCommandLine
    //   -uabModel Assets/Models/Hero.fbx -uabClips "Assets/Anim/Run.anim;Assets/Anim/Walk.fbx" -uabOut C:/out/baked.json [-uabFps 30]
    public static void BakeFromCommandLine()
    {
        var args = Environment.GetCommandLineArgs();
        string Arg(string name)
        {
            int i = Array.IndexOf(args, name);
            return i >= 0 && i + 1 < args.Length ? args[i + 1] : null;
        }
        string modelPath = Arg("-uabModel"), clipList = Arg("-uabClips"), outPath = Arg("-uabOut");
        float fps = float.TryParse(Arg("-uabFps") ?? "0", NumberStyles.Float, CultureInfo.InvariantCulture, out var f) ? f : 0f;
        if (modelPath == null || clipList == null || outPath == null)
            throw new ArgumentException("need -uabModel, -uabClips and -uabOut");
        var model = AssetDatabase.LoadAssetAtPath<GameObject>(modelPath) ?? throw new ArgumentException("model not found: " + modelPath);
        var clips = new List<AnimationClip>();
        foreach (var p in clipList.Split(';').Select(s => s.Trim()).Where(s => s.Length > 0))
        {
            var found = AssetDatabase.LoadAllAssetsAtPath(p).OfType<AnimationClip>().Where(c => !c.name.StartsWith("__preview__")).ToList();
            if (found.Count == 0) throw new ArgumentException("no AnimationClip in " + p);
            clips.AddRange(found);
        }
        Bake(model, clips, fps, outPath);
    }

    public static void Bake(GameObject model, List<AnimationClip> clips, float fpsOverride, string outPath)
    {
        if (AnimationMode.InAnimationMode())
            throw new InvalidOperationException("AnimationMode is already in use by another tool; close the Animation window and retry");

        var instance = (GameObject)UnityEngine.Object.Instantiate(model);
        instance.hideFlags = HideFlags.HideAndDontSave;
        var sb = new StringBuilder();
        try
        {
            var importer = AssetImporter.GetAtPath(AssetDatabase.GetAssetPath(model)) as ModelImporter;
            if (importer != null && importer.optimizeGameObjects)
                AnimatorUtility.DeoptimizeTransformHierarchy(instance);
            if (importer != null && importer.bakeAxisConversion)
                Debug.LogWarning("UnityAnimBridge: bakeAxisConversion is on; uab will reject this model (DESIGN 8.3)");

            var transforms = instance.GetComponentsInChildren<Transform>(true);
            var paths = transforms.Select(t => PathOf(t, instance.transform)).ToArray();
            var animator = instance.GetComponent<Animator>();

            sb.Append("{\"uab_baked\":").Append(BakedVersion)
              .Append(",\"unity_version\":").Append(Str(Application.unityVersion))
              .Append(",\"model\":").Append(Str(AssetDatabase.GetAssetPath(model)))
              .Append(",\"fps\":").Append(Num(fpsOverride))
              .Append(",\"rest\":{");
            for (int i = 0; i < transforms.Length; i++)
            {
                if (i > 0) sb.Append(',');
                sb.Append(Str(paths[i])).Append(':');
                AppendTrs(sb, transforms[i]);
            }
            sb.Append("},\"clips\":[");

            AnimationMode.StartAnimationMode();
            try
            {
                bool first = true;
                foreach (var clip in clips)
                {
                    if (clip.humanMotion && (animator == null || animator.avatar == null || !animator.avatar.isHuman))
                    {
                        Debug.LogError($"UnityAnimBridge: {clip.name} is Humanoid but the model has no Humanoid Avatar; skipped");
                        continue;
                    }
                    if (!first) sb.Append(',');
                    first = false;
                    BakeClip(sb, instance, clip, transforms, paths, fpsOverride);
                }
            }
            finally
            {
                AnimationMode.StopAnimationMode();
            }
            sb.Append("]}");
        }
        finally
        {
            UnityEngine.Object.DestroyImmediate(instance);
        }
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(outPath)));
        File.WriteAllText(outPath, sb.ToString(), new UTF8Encoding(false));
        Debug.Log($"UnityAnimBridge: baked {clips.Count} clip(s) to {outPath}");
    }

    static void BakeClip(StringBuilder sb, GameObject go, AnimationClip clip, Transform[] transforms, string[] paths, float fpsOverride)
    {
        float fps = fpsOverride > 0 ? fpsOverride : (clip.frameRate > 0 ? clip.frameRate : 30f);
        float d = Mathf.Max(0f, clip.length);
        int n = d <= 0 ? 1 : Mathf.Max(2, Mathf.RoundToInt(d * fps) + 1);
        var t = new List<Vector3>[transforms.Length];
        var r = new List<Quaternion>[transforms.Length];
        var s = new List<Vector3>[transforms.Length];
        for (int i = 0; i < transforms.Length; i++) { t[i] = new List<Vector3>(n); r[i] = new List<Quaternion>(n); s[i] = new List<Vector3>(n); }

        for (int f = 0; f < n; f++)
        {
            float time = Mathf.Min(f / fps, d);  // Match the converter's uniform sample grid.
            AnimationMode.BeginSampling();
            try { AnimationMode.SampleAnimationClip(go, clip, time); }
            finally { AnimationMode.EndSampling(); }
            for (int i = 0; i < transforms.Length; i++)
            {
                t[i].Add(transforms[i].localPosition);
                r[i].Add(transforms[i].localRotation);
                s[i].Add(transforms[i].localScale);
            }
        }
        float outD = n > 1 ? (n - 1) / fps : 0f;
        var settings = AnimationUtility.GetAnimationClipSettings(clip);
        sb.Append("{\"name\":").Append(Str(clip.name))
          .Append(",\"fps\":").Append(Num(fps))
          .Append(",\"frames\":").Append(n)
          .Append(",\"duration\":").Append(Num(outD))
          .Append(",\"loop\":").Append(settings.loopTime ? "true" : "false")
          .Append(",\"events\":[");
        var events = AnimationUtility.GetAnimationEvents(clip);
        for (int e = 0; e < events.Length; e++)
        {
            if (e > 0) sb.Append(',');
            var ev = events[e];
            sb.Append("{\"time\":").Append(Num(Mathf.Min(ev.time, outD)))
              .Append(",\"name\":").Append(Str(ev.functionName))
              .Append(",\"string\":").Append(Str(ev.stringParameter ?? ""))
              .Append(",\"float\":").Append(Num(ev.floatParameter))
              .Append(",\"int\":").Append(ev.intParameter).Append('}');
        }
        sb.Append("],\"tracks\":{");
        for (int i = 0; i < transforms.Length; i++)
        {
            if (i > 0) sb.Append(',');
            sb.Append(Str(paths[i])).Append(":{\"t\":");
            AppendRows(sb, t[i].Select(v => new[] { v.x, v.y, v.z }));
            sb.Append(",\"r\":");
            AppendRows(sb, r[i].Select(q => new[] { q.x, q.y, q.z, q.w }));
            sb.Append(",\"s\":");
            AppendRows(sb, s[i].Select(v => new[] { v.x, v.y, v.z }));
            sb.Append('}');
        }
        sb.Append("}}");
    }

    static string PathOf(Transform t, Transform root)
    {
        var parts = new List<string>();
        for (var c = t; c != null && c != root; c = c.parent) parts.Add(c.name);
        parts.Reverse();
        return string.Join("/", parts);
    }

    static void AppendTrs(StringBuilder sb, Transform t)
    {
        var p = t.localPosition; var q = t.localRotation; var s = t.localScale;
        sb.Append("{\"t\":[").Append(Num(p.x)).Append(',').Append(Num(p.y)).Append(',').Append(Num(p.z))
          .Append("],\"r\":[").Append(Num(q.x)).Append(',').Append(Num(q.y)).Append(',').Append(Num(q.z)).Append(',').Append(Num(q.w))
          .Append("],\"s\":[").Append(Num(s.x)).Append(',').Append(Num(s.y)).Append(',').Append(Num(s.z)).Append("]}");
    }

    static void AppendRows(StringBuilder sb, IEnumerable<float[]> rows)
    {
        sb.Append('[');
        bool first = true;
        foreach (var row in rows)
        {
            if (!first) sb.Append(',');
            first = false;
            sb.Append('[');
            for (int i = 0; i < row.Length; i++) { if (i > 0) sb.Append(','); sb.Append(Num(row[i])); }
            sb.Append(']');
        }
        sb.Append(']');
    }

    static string Num(float v) => float.IsFinite(v) ? v.ToString("R", CultureInfo.InvariantCulture) : "null";

    static string Str(string s)
    {
        var sb = new StringBuilder("\"");
        foreach (char c in s)
        {
            switch (c)
            {
                case '"': sb.Append("\\\""); break;
                case '\\': sb.Append("\\\\"); break;
                case '\n': sb.Append("\\n"); break;
                case '\r': sb.Append("\\r"); break;
                case '\t': sb.Append("\\t"); break;
                default:
                    if (c < 0x20) sb.Append("\\u").Append(((int)c).ToString("x4"));
                    else sb.Append(c);
                    break;
            }
        }
        return sb.Append('"').ToString();
    }
}
