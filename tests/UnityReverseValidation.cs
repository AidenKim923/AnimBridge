using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEngine;

// Read AnimationEvent times in seconds and sample saved Generic clips
// against their imported model with the existing baker.
public static class UnityReverseValidation
{
    [Serializable] public class Marker { public string name, function, messageOptions; public float time; }
    [Serializable] public class Item { public string id, baked; public Marker[] events, embeddedEvents; }
    [Serializable] public class Evidence { public List<Item> clips = new List<Item>(); }

    public static void Run()
    {
        string[] args = Environment.GetCommandLineArgs();
        string Arg(string key) { int i = Array.IndexOf(args, key); return i >= 0 && i + 1 < args.Length ? args[i + 1] : null; }
        string reportPath = Arg("-animBridgeReport");
        string output = Arg("-animBridgeValidation");
        AnimBridgeImporter.ImportFromCommandLine();
        var report = JsonUtility.FromJson<AnimBridgeImporter.Report>(File.ReadAllText(reportPath));
        var manifest = JsonUtility.FromJson<AnimBridgeImporter.Manifest>(File.ReadAllText(Arg("-animBridgeManifest")));
        var evidence = new Evidence();
        for (int i = 0; i < report.clips.Count; i++)
        {
            var result = report.clips[i];
            var input = manifest.clips.Single(value => value.id == result.id);
            var clip = AssetDatabase.LoadAssetAtPath<AnimationClip>(result.animation_asset_path);
            var embedded = AssetDatabase.LoadAllAssetsAtPath(result.asset_path).OfType<AnimationClip>()
                .Single(value => !value.name.StartsWith("__preview__", StringComparison.Ordinal));
            var model = AssetDatabase.LoadAssetAtPath<GameObject>(result.asset_path);
            string baked = Path.Combine(Path.GetDirectoryName(output), "reverse_pose_" + i + ".json");
            UnityAnimBridgeBaker.Bake(model, new List<AnimationClip> { clip }, input.fps, baked);
            evidence.clips.Add(new Item { id = result.id, baked = baked,
                events = ReadMarkers(clip), embeddedEvents = ReadMarkers(embedded) });
        }
        File.WriteAllText(output, JsonUtility.ToJson(evidence, true));
        Debug.Log("ANIMBRIDGE_V6_COMPLETE " + output);
    }

    static Marker[] ReadMarkers(AnimationClip clip)
    {
        return AnimationUtility.GetAnimationEvents(clip).Select(marker => new Marker
        { name = marker.stringParameter, function = marker.functionName,
          messageOptions = marker.messageOptions.ToString(), time = marker.time }).ToArray();
    }
}
