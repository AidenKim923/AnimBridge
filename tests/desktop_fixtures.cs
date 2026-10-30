// Test-owned fixture generator. Only this known source is installed in its own
// fixture project; package Editor scripts are never copied or executed.
using System;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEngine;

public static class DesktopFixtureBuilder
{
    public static void Run()
    {
        string[] args = Environment.GetCommandLineArgs();
        string Arg(string key) { int i = Array.IndexOf(args, key); return args[i + 1]; }
        EditorSettings.serializationMode = SerializationMode.ForceText;
        string path = Arg("-desktopFixtureSource");
        var source = AssetDatabase.LoadAllAssetsAtPath(path).OfType<AnimationClip>()
            .Single(clip => !clip.name.StartsWith("__preview__", StringComparison.Ordinal));
        if (!source.humanMotion) throw new InvalidOperationException("Expected real Humanoid source animation");
        var clip = UnityEngine.Object.Instantiate(source);
        clip.hideFlags = HideFlags.None;
        AssetDatabase.CreateAsset(clip, "Assets/Data/Human.anim");
        AssetDatabase.SaveAssets();
        File.Copy("Assets/Data/Human.anim", Arg("-desktopFixtureOut"), true);
        Debug.Log("DESKTOP_HUMAN_FIXTURE " + clip.name);
    }
}
