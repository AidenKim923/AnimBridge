// Copy into an Editor folder. Imports AnimBridge Unreal FBX exports as Generic clips.
// Unity.exe -batchmode -quit -projectPath <project> -executeMethod AnimBridgeImporter.ImportFromCommandLine
//   -animBridgeManifest <animbridge-manifest.json> -animBridgeDest Assets/AnimBridge [-animBridgeReport <json>]
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using UnityEditor;
using UnityEngine;

public static class AnimBridgeImporter
{
    [Serializable] public class EventData { public string name; public float time; }
    [Serializable] public class ClipInput
    {
        public string id, name, fbx, source_asset;
        public float fps, duration;
        public int frames;
        public EventData[] events;
    }
    [Serializable] public class Manifest
    {
        public int animbridge_version;
        public string source_engine;
        public ClipInput[] clips;
    }
    [Serializable] public class ImportedClip { public string name; public float length, fps; }
    [Serializable] public class Result
    {
        public string id, name, asset_path, animation_asset_path, status, message;
        public ImportedClip[] clips;
    }
    [Serializable] public class Report
    {
        public int animbridge_version = 1;
        public string source_engine = "unreal", unity_version = Application.unityVersion;
        public List<Result> clips = new List<Result>();
    }

    [MenuItem("Tools/AnimBridge/Import Unreal Manifest...")]
    public static void ImportMenu()
    {
        string manifest = EditorUtility.OpenFilePanel("AnimBridge Unreal export manifest", "", "json");
        if (!string.IsNullOrEmpty(manifest)) Import(manifest, "Assets/AnimBridge", null);
    }

    public static void ImportFromCommandLine()
    {
        string[] args = Environment.GetCommandLineArgs();
        string Arg(string key)
        {
            int i = Array.IndexOf(args, key);
            return i >= 0 && i + 1 < args.Length ? args[i + 1] : null;
        }
        string manifest = Arg("-animBridgeManifest");
        if (string.IsNullOrEmpty(manifest)) throw new ArgumentException("Missing -animBridgeManifest");
        Import(manifest, Arg("-animBridgeDest") ?? "Assets/AnimBridge", Arg("-animBridgeReport"));
    }

    public static Report Import(string manifestPath, string destination, string reportPath)
    {
        manifestPath = Path.GetFullPath(manifestPath);
        destination = destination.Replace('\\', '/').TrimEnd('/');
        string project = Path.GetFullPath(Path.Combine(Application.dataPath, ".."));
        string assetRoot = Path.GetFullPath(Application.dataPath) + Path.DirectorySeparatorChar;
        string targetRoot = Path.GetFullPath(Path.Combine(project, destination));
        if (!destination.StartsWith("Assets/", StringComparison.Ordinal) ||
            !targetRoot.StartsWith(assetRoot, StringComparison.OrdinalIgnoreCase))
            throw new ArgumentException("Destination must be a folder inside this project's Assets directory");
        Manifest manifest = JsonUtility.FromJson<Manifest>(File.ReadAllText(manifestPath));
        if (manifest == null || manifest.animbridge_version != 1 || manifest.source_engine != "unreal" || manifest.clips == null)
            throw new ArgumentException("Expected an AnimBridge v1 source_engine=unreal manifest");
        if (manifest.clips.Length == 0) throw new ArgumentException("Manifest contains no animations");
        Directory.CreateDirectory(targetRoot);
        AssetDatabase.Refresh(ImportAssetOptions.ForceSynchronousImport);
        var report = new Report();
        foreach (ClipInput input in manifest.clips)
        {
            var result = new Result { id = input.id, name = input.name, status = "error", clips = Array.Empty<ImportedClip>() };
            report.clips.Add(result);
            try
            {
                if (string.IsNullOrEmpty(input.fbx)) throw new ArgumentException("Missing FBX path");
                string source = Path.GetFullPath(Path.IsPathRooted(input.fbx) ? input.fbx : Path.Combine(Path.GetDirectoryName(manifestPath), input.fbx));
                if (!source.EndsWith(".fbx", StringComparison.OrdinalIgnoreCase) || !File.Exists(source))
                    throw new FileNotFoundException("Exported FBX is missing", source);
                string stem = SafeName(Path.GetFileNameWithoutExtension(source));
                // Stable source identity prevents two Unreal packages with the same name colliding.
                using (var sha = SHA256.Create())
                    stem += "_" + BitConverter.ToString(sha.ComputeHash(Encoding.UTF8.GetBytes(input.id ?? input.source_asset ?? source))).Replace("-", "").Substring(0, 10).ToLowerInvariant();
                string assetPath = destination + "/" + stem + ".fbx";
                string output = Path.GetFullPath(Path.Combine(project, assetPath));
                if (!string.Equals(source, output, StringComparison.OrdinalIgnoreCase)) File.Copy(source, output, true);
                AssetDatabase.ImportAsset(assetPath, ImportAssetOptions.ForceSynchronousImport | ImportAssetOptions.ForceUpdate);
                var importer = AssetImporter.GetAtPath(assetPath) as ModelImporter;
                if (importer == null) throw new InvalidOperationException("FBX did not create a ModelImporter");
                importer.animationType = ModelImporterAnimationType.Generic;
                importer.avatarSetup = ModelImporterAvatarSetup.CreateFromThisModel;
                importer.importAnimation = true;
                importer.optimizeGameObjects = false;
                importer.preserveHierarchy = true;
                importer.useFileScale = true;
                importer.globalScale = 1;
                importer.bakeAxisConversion = false;
                importer.animationCompression = ModelImporterAnimationCompression.Off;
                importer.resampleCurves = true;
                importer.SaveAndReimport();
                ModelImporterClipAnimation[] settings = importer.defaultClipAnimations;
                if (settings.Length != 1) throw new InvalidOperationException("Expected one exported take; found " + settings.Length);
                AnimationClip[] defaultClips = AssetDatabase.LoadAllAssetsAtPath(assetPath).OfType<AnimationClip>()
                    .Where(value => !value.name.StartsWith("__preview__", StringComparison.Ordinal)).ToArray();
                if (defaultClips.Length != 1) throw new InvalidOperationException("Expected one default AnimationClip");
                float takeDuration = defaultClips[0].length;
                if (!string.IsNullOrEmpty(input.name)) settings[0].name = input.name;
                settings[0].events = (input.events ?? Array.Empty<EventData>()).Select(ev => new AnimationEvent
                {
                    // ModelImporterClipAnimation uses normalized take time;
                    // GetAnimationEvents on the resulting clip returns seconds.
                    functionName = "AnimBridgeSyncMarker", time = takeDuration > 0 ? Mathf.Clamp01(ev.time / takeDuration) : 0,
                    stringParameter = ev.name ?? "Event", messageOptions = SendMessageOptions.DontRequireReceiver
                }).ToArray();
                importer.clipAnimations = settings;
                importer.SaveAndReimport();
                AnimationClip[] clips = AssetDatabase.LoadAllAssetsAtPath(assetPath).OfType<AnimationClip>()
                    .Where(clip => !clip.name.StartsWith("__preview__", StringComparison.Ordinal)).ToArray();
                if (clips.Length != 1) throw new InvalidOperationException("Expected one AnimationClip; found " + clips.Length);
                AnimationClip clip = clips[0];
                if (AnimationUtility.GetCurveBindings(clip).Length == 0)
                    throw new InvalidOperationException("Imported clip contains no animation curves");
                float tolerance = 1.5f / Mathf.Max(input.fps, 1);
                if (input.duration > 0 && Mathf.Abs(clip.length - input.duration) > tolerance)
                    throw new InvalidOperationException("Imported length " + clip.length + " differs from exported " + input.duration);
                // Unreal's FBX exporter writes a nominal 30 fps time mode even
                // for 10 fps source clips. A standalone clip can retain the
                // source rate without changing any curve timestamps.
                string animationPath = destination + "/" + stem + ".anim";
                var saved = AssetDatabase.LoadMainAssetAtPath(animationPath);
                if (saved != null && !(saved is AnimationClip))
                    throw new InvalidOperationException("Animation output path already contains another asset: " + animationPath);
                AnimationClip standalone = saved as AnimationClip;
                if (standalone == null)
                {
                    standalone = UnityEngine.Object.Instantiate(clip);
                    AssetDatabase.CreateAsset(standalone, animationPath);
                }
                else EditorUtility.CopySerialized(clip, standalone);
                standalone.name = input.name ?? clip.name;
                if (input.fps > 0 && !float.IsNaN(input.fps) && !float.IsInfinity(input.fps)) standalone.frameRate = input.fps;
                AnimationEvent[] standaloneEvents = AnimationUtility.GetAnimationEvents(standalone);
                foreach (AnimationEvent marker in standaloneEvents) marker.messageOptions = SendMessageOptions.DontRequireReceiver;
                AnimationUtility.SetAnimationEvents(standalone, standaloneEvents);
                EditorUtility.SetDirty(standalone);
                result.asset_path = assetPath;
                result.animation_asset_path = animationPath;
                result.clips = new[] { new ImportedClip { name = standalone.name, length = standalone.length, fps = standalone.frameRate } };
                result.status = "ok";
                result.message = "Generic animation imported; Sync Markers are AnimationEvents named AnimBridgeSyncMarker with marker name in stringParameter.";
            }
            catch (Exception error) { result.message = error.Message; Debug.LogError("AnimBridge: " + input.name + ": " + error); }
        }
        AssetDatabase.SaveAssets();
        reportPath = Path.GetFullPath(reportPath ?? Path.Combine(Path.GetDirectoryName(manifestPath), "animbridge-unity-import-report.json"));
        Directory.CreateDirectory(Path.GetDirectoryName(reportPath));
        File.WriteAllText(reportPath, JsonUtility.ToJson(report, true), new UTF8Encoding(false));
        Debug.Log("AnimBridge import report: " + reportPath);
        if (report.clips.Any(result => result.status != "ok")) throw new InvalidOperationException("One or more imports failed; see " + reportPath);
        return report;
    }

    static string SafeName(string name)
    {
        foreach (char c in Path.GetInvalidFileNameChars()) name = name.Replace(c, '_');
        name = name.Trim(' ', '.');
        return string.IsNullOrEmpty(name) ? "Animation" : name;
    }
}
