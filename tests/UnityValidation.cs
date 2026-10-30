// Unity test harness, installed only in an isolated project by test_unity.py.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEngine;

public static class UnityValidation
{
    [Serializable] public class Item
    {
        public string label, model, clip, baked;
        public float fileScale, globalScale;
        public bool useFileScale, preserveHierarchy;
        public string[] paths;
    }
    [Serializable] public class HumanResult { public bool valid, human, clipHuman; public string name; public string[] curveNames; public float humanScale; public float[] rootTStart, rootTEnd, rootQStart, rootQEnd; }
    [Serializable] public class Summary { public string unity; public List<Item> items = new List<Item>(); public List<HumanResult> humanoid = new List<HumanResult>(); public string offsetTake; public float offsetFirst, offsetLast; }

    public static void Run()
    {
        AssetDatabase.Refresh();
        EditorSettings.serializationMode = SerializationMode.ForceText;
        string root = Path.GetFullPath(Path.Combine(Application.dataPath, "../.."));
        var summary = new Summary { unity = Application.unityVersion };
        foreach (string kind in new[] { "zup_cm", "yup_m" })
        foreach (bool preserve in new[] { false, true })
        {
            string modelPath = "Assets/" + kind + ".fbx";
            var importer = (ModelImporter)AssetImporter.GetAtPath(modelPath);
            importer.animationType = ModelImporterAnimationType.Generic;
            importer.importAnimation = true;
            importer.optimizeGameObjects = false;
            importer.bakeAxisConversion = false;
            importer.preserveHierarchy = preserve;
            importer.globalScale = 1;
            importer.useFileScale = true;
            importer.animationCompression = ModelImporterAnimationCompression.Off;
            importer.SaveAndReimport();
            var model = AssetDatabase.LoadAssetAtPath<GameObject>(modelPath);
            string label = kind + (preserve ? "_preserve" : "_default");
            var clip = MakeClip(model, label);
            string clipPath = "Assets/" + label + ".anim";
            AssetDatabase.CreateAsset(clip, clipPath);
            AssetDatabase.SaveAssets();
            string baked = Path.Combine(root, label + "_baked.json");
            UnityAnimBridgeBaker.Bake(model, new List<AnimationClip> { clip }, 10, baked);
            summary.items.Add(new Item { label = label, model = modelPath, clip = clipPath, baked = baked,
                fileScale = importer.fileScale, globalScale = importer.globalScale, useFileScale = importer.useFileScale,
                preserveHierarchy = importer.preserveHierarchy,
                paths = model.GetComponentsInChildren<Transform>(true).Select(t => AnimationUtility.CalculateTransformPath(t, model.transform)).ToArray() });
        }
        ConfigureOffset(summary, root);
        ConfigureHuman(summary, root);
        File.WriteAllText(Path.Combine(root, "unity_summary.json"), JsonUtility.ToJson(summary, true));
        Debug.Log("UAB_V3_COMPLETE " + JsonUtility.ToJson(summary));
    }

    static AnimationClip MakeClip(GameObject model, string name)
    {
        var clip = new AnimationClip { name = name, frameRate = 10 };
        foreach (string bone in new[] { "Hips", "Spine", "Hand" })
        {
            Transform tr = model.GetComponentsInChildren<Transform>(true).Single(t => t.name == bone);
            string path = AnimationUtility.CalculateTransformPath(tr, model.transform);
            int count = bone == "Spine" ? 4 : 3;
            string attr = bone == "Hips" ? "m_LocalPosition." : bone == "Spine" ? "m_LocalRotation." : "m_LocalScale.";
            var keys = Enumerable.Range(0, count).Select(_ => new List<Keyframe>()).ToArray();
            for (int f = 0; f <= 10; f++)
            {
                float t = f / 10f;
                Vector3 p = tr.localPosition + new Vector3(.1f * t, .05f * t, .02f * t);
                Quaternion q = tr.localRotation * Quaternion.AngleAxis(45 * t, new Vector3(.3f, .8f, .52f).normalized);
                Vector3 scale = tr.localScale + new Vector3(.05f * t, .02f * t, .01f * t);
                float[] values = bone == "Hips" ? new[] { p.x, p.y, p.z } : bone == "Spine" ? new[] { q.x, q.y, q.z, q.w } : new[] { scale.x, scale.y, scale.z };
                for (int c = 0; c < count; c++) keys[c].Add(new Keyframe(t, values[c], 0, 0));
            }
            for (int c = 0; c < count; c++)
                AnimationUtility.SetEditorCurve(clip, EditorCurveBinding.FloatCurve(path, typeof(Transform), attr + "xyzw"[c]), new AnimationCurve(keys[c].ToArray()));
        }
        var settings = AnimationUtility.GetAnimationClipSettings(clip);
        settings.startTime = 0; settings.stopTime = 1;
        AnimationUtility.SetAnimationClipSettings(clip, settings);
        clip.EnsureQuaternionContinuity();
        return clip;
    }

    static void ConfigureOffset(Summary summary, string root)
    {
        string path = "Assets/offset_take.fbx";
        var importer = (ModelImporter)AssetImporter.GetAtPath(path);
        importer.animationType = ModelImporterAnimationType.Generic;
        importer.animationCompression = ModelImporterAnimationCompression.Off;
        importer.importAnimation = true;
        importer.SaveAndReimport();
        var take = importer.defaultClipAnimations.Single();
        summary.offsetTake = take.takeName;
        summary.offsetFirst = take.firstFrame; summary.offsetLast = take.lastFrame;
        take.name = "OffsetMid"; take.firstFrame = 54; take.lastFrame = 66;
        importer.clipAnimations = new[] { take };
        importer.SaveAndReimport();
        var model = AssetDatabase.LoadAssetAtPath<GameObject>(path);
        var clips = AssetDatabase.LoadAllAssetsAtPath(path).OfType<AnimationClip>().Where(c => !c.name.StartsWith("__preview__")).ToList();
        UnityAnimBridgeBaker.Bake(model, clips, 24, Path.Combine(root, "offset_baked.json"));
    }

    static void ConfigureHuman(Summary summary, string root)
    {
        string path = "Assets/humanoid.fbx";
        var importer = (ModelImporter)AssetImporter.GetAtPath(path);
        var model = AssetDatabase.LoadAssetAtPath<GameObject>(path);
        string[] names = { "Hips", "Spine", "Chest", "Neck", "Head", "LeftUpperArm", "LeftLowerArm", "LeftHand", "RightUpperArm", "RightLowerArm", "RightHand", "LeftUpperLeg", "LeftLowerLeg", "LeftFoot", "RightUpperLeg", "RightLowerLeg", "RightFoot" };
        var human = names.Select(n => new HumanBone { boneName = n, humanName = n, limit = new HumanLimit { useDefaultValues = true } }).ToArray();
        var skeleton = model.GetComponentsInChildren<Transform>(true).Select(t => new SkeletonBone { name = t.name, position = t.localPosition, rotation = t.localRotation, scale = t.localScale }).ToArray();
        importer.animationType = ModelImporterAnimationType.Human;
        importer.avatarSetup = ModelImporterAvatarSetup.CreateFromThisModel;
        importer.humanDescription = new HumanDescription { human = human, skeleton = skeleton, armStretch = .05f, legStretch = .05f, upperArmTwist = .5f, lowerArmTwist = .5f, upperLegTwist = .5f, lowerLegTwist = .5f, feetSpacing = 0 };
        importer.animationCompression = ModelImporterAnimationCompression.Off;
        importer.SaveAndReimport();
        foreach (bool bakeRoot in new[] { false, true })
        {
            var take = importer.defaultClipAnimations.Single();
            take.name = bakeRoot ? "HumanBakedRoot" : "HumanExtractedRoot";
            take.lockRootPositionXZ = bakeRoot; take.lockRootHeightY = bakeRoot; take.lockRootRotation = bakeRoot;
            take.keepOriginalPositionXZ = true; take.keepOriginalPositionY = true; take.keepOriginalOrientation = true;
            importer.clipAnimations = new[] { take };
            importer.SaveAndReimport();
            model = AssetDatabase.LoadAssetAtPath<GameObject>(path);
            var avatar = model.GetComponent<Animator>().avatar;
            var clips = AssetDatabase.LoadAllAssetsAtPath(path).OfType<AnimationClip>().Where(c => !c.name.StartsWith("__preview__")).ToList();
            var clip = clips.Single();
            Func<string, int, float, float[]> eval = (prefix, count, time) => Enumerable.Range(0, count).Select(i => AnimationUtility.GetEditorCurve(clip, EditorCurveBinding.FloatCurve("", typeof(Animator), prefix + "." + "xyzw"[i])).Evaluate(time)).ToArray();
            summary.humanoid.Add(new HumanResult { valid = avatar.isValid, human = avatar.isHuman, clipHuman = clip.humanMotion, name = take.name,
                humanScale = model.GetComponent<Animator>().humanScale,
                curveNames = AnimationUtility.GetCurveBindings(clip).Select(b => b.propertyName).ToArray(),
                rootTStart = eval("RootT", 3, 0), rootTEnd = eval("RootT", 3, clip.length), rootQStart = eval("RootQ", 4, 0), rootQEnd = eval("RootQ", 4, clip.length) });
            UnityAnimBridgeBaker.Bake(model, clips, 24, Path.Combine(root, take.name + "_baked.json"));
        }
    }
}
