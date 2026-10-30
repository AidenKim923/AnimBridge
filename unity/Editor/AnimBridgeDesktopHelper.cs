// Runs only in an app-owned, freshly created project with approved FBX/anim/meta
// data and these trusted helpers. Never copy package scripts or plugins here.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEngine;

public static class AnimBridgeDesktopHelper
{
    const string Version = "1.0";
    [Serializable] public class HumanMapping { public string human, bone; }
    [Serializable] public class Request
    {
        public string operation, session, helper_version, model, clip, clip_name, baked_file, preview_file;
        public bool humanoid, include_mesh;
        public float fps;
        public int max_frames = 121, max_mesh_vertices = 1800, max_point_frames = 150000;
        public HumanMapping[] human_mapping;
    }
    [Serializable] public class AvatarInfo { public bool exists, is_valid, is_human; }
    [Serializable] public class BoneInfo
    {
        public string name, path, parent;
        public float[] rest_t, rest_r, rest_s;
    }
    [Serializable] public class Response
    {
        public int schema_version = 1;
        public string status = "error", helper_version = Version, unity_version = Application.unityVersion;
        public string session, project, reason, baked_file, clip_name, preview_file;
        public bool connected = true, clip_humanoid;
        public AvatarInfo avatar = new AvatarInfo();
        public BoneInfo[] bones = Array.Empty<BoneInfo>();
        public string[] duplicates = Array.Empty<string>(), required_missing = Array.Empty<string>(), errors = Array.Empty<string>();
        public string[] capabilities = { "generic-bake", "humanoid-bake-with-valid-avatar", "avatar-validation", "isolated-project" };
    }

    static string DataPath(string path)
    {
        if (string.IsNullOrEmpty(path) || !path.StartsWith("Assets/Data/", StringComparison.Ordinal) ||
            path.Contains("..") || path.Contains('\\')) throw new ArgumentException("Only isolated Assets/Data paths are accepted");
        return path;
    }

    public static void Run()
    {
        string[] args = Environment.GetCommandLineArgs();
        string Arg(string key) { int index = Array.IndexOf(args, key); return index >= 0 && index + 1 < args.Length ? args[index + 1] : null; }
        string output = Arg("-animBridgeDesktopResult");
        var response = new Response { project = Path.GetFullPath(Path.Combine(Application.dataPath, "..")) };
        try
        {
            var request = JsonUtility.FromJson<Request>(File.ReadAllText(Arg("-animBridgeDesktopRequest")));
            response.session = request.session;
            if (request.helper_version != Version) throw new ArgumentException("Helper version mismatch");
            if (request.operation == "test")
            {
                response.status = "connected";
                response.reason = "Isolated Unity helper session verified";
            }
            else
            {
                string modelPath = DataPath(request.model);
                var importer = AssetImporter.GetAtPath(modelPath) as ModelImporter;
                if (importer == null) throw new ArgumentException("Model FBX importer not found");
                bool reimport = importer.optimizeGameObjects || importer.animationCompression != ModelImporterAnimationCompression.Off;
                importer.optimizeGameObjects = false;
                importer.animationCompression = ModelImporterAnimationCompression.Off;
                bool mappingChanged = request.human_mapping != null && request.human_mapping.Length > 0 &&
                    (importer.animationType != ModelImporterAnimationType.Human || !request.human_mapping.All(mapping =>
                        importer.humanDescription.human.Any(bone => bone.humanName == mapping.human && bone.boneName == mapping.bone)));
                if (mappingChanged)
                {
                    reimport = true;
                    var source = AssetDatabase.LoadAssetAtPath<GameObject>(modelPath);
                    importer.animationType = ModelImporterAnimationType.Human;
                    importer.avatarSetup = ModelImporterAvatarSetup.CreateFromThisModel;
                    importer.humanDescription = new HumanDescription
                    {
                        human = request.human_mapping.Select(mapping => new HumanBone { humanName = mapping.human,
                            boneName = mapping.bone, limit = new HumanLimit { useDefaultValues = true } }).ToArray(),
                        skeleton = source.GetComponentsInChildren<Transform>(true).Select(transform => new SkeletonBone
                        { name = transform.name, position = transform.localPosition, rotation = transform.localRotation, scale = transform.localScale }).ToArray(),
                        armStretch = .05f, legStretch = .05f, upperArmTwist = .5f, lowerArmTwist = .5f,
                        upperLegTwist = .5f, lowerLegTwist = .5f, feetSpacing = 0
                    };
                }
                if (reimport) importer.SaveAndReimport();
                var model = AssetDatabase.LoadAssetAtPath<GameObject>(modelPath);
                var transforms = model.GetComponentsInChildren<Transform>(true);
                response.bones = transforms.Select(transform => new BoneInfo { name = transform.name,
                    path = AnimationUtility.CalculateTransformPath(transform, model.transform),
                    parent = transform.parent == null ? null : AnimationUtility.CalculateTransformPath(transform.parent, model.transform),
                    rest_t = new[] { transform.localPosition.x, transform.localPosition.y, transform.localPosition.z },
                    rest_r = new[] { transform.localRotation.x, transform.localRotation.y, transform.localRotation.z, transform.localRotation.w },
                    rest_s = new[] { transform.localScale.x, transform.localScale.y, transform.localScale.z } }).ToArray();
                response.duplicates = transforms.GroupBy(transform => transform.name).Where(group => group.Count() > 1).Select(group => group.Key).ToArray();
                var invalidRest = response.bones.Where(bone => bone.rest_t.Concat(bone.rest_r).Concat(bone.rest_s).Any(value => float.IsNaN(value) || float.IsInfinity(value)) ||
                    bone.rest_s.Any(value => Math.Abs(value) < 1e-8f)).Select(bone => bone.path).ToArray();
                if (invalidRest.Length > 0) throw new InvalidOperationException("Invalid rest transforms: " + string.Join(", ", invalidRest));
                if (response.duplicates.Length > 0) throw new InvalidOperationException("Duplicate Transform names: " + string.Join(", ", response.duplicates));
                var animator = model.GetComponent<Animator>();
                Avatar avatar = animator == null ? null : animator.avatar;
                response.avatar.exists = avatar != null;
                response.avatar.is_valid = avatar != null && avatar.isValid;
                response.avatar.is_human = avatar != null && avatar.isHuman;
                List<AnimationClip> clips = new List<AnimationClip>();
                if (!string.IsNullOrEmpty(request.clip))
                {
                    clips = AssetDatabase.LoadAllAssetsAtPath(DataPath(request.clip)).OfType<AnimationClip>()
                        .Where(clip => !clip.name.StartsWith("__preview__", StringComparison.Ordinal) &&
                            (string.IsNullOrEmpty(request.clip_name) || clip.name == request.clip_name)).ToList();
                    if (clips.Count != 1) throw new ArgumentException("Choose one unambiguous AnimationClip; found " + clips.Count);
                    response.clip_name = clips[0].name;
                    response.clip_humanoid = clips[0].humanMotion;
                    if (request.humanoid && !response.clip_humanoid)
                        throw new InvalidOperationException("The selected clip was not imported as Humanoid. Resolve its source Avatar before baking; a valid target Avatar alone is insufficient.");
                }
                bool needsHuman = request.humanoid || response.clip_humanoid;
                if (needsHuman)
                {
                    var missing = new List<string>();
                    for (int i = 0; i < HumanTrait.BoneCount; i++)
                    {
                        if (HumanTrait.RequiredBone(i) && (!response.avatar.is_human || animator.GetBoneTransform((HumanBodyBones)i) == null))
                            missing.Add(HumanTrait.BoneName[i]);
                    }
                    response.required_missing = missing.ToArray();
                    if (!response.avatar.is_valid || !response.avatar.is_human || response.required_missing.Length > 0)
                    {
                        response.status = "needsAvatar";
                        response.reason = "Unity did not validate a complete Humanoid Avatar";
                    }
                    else response.status = "ok";
                }
                else response.status = "ok";
                if (request.operation == "bake" && response.status == "ok")
                {
                    if (clips.Count != 1) throw new ArgumentException("A clip is required for bake");
                    UnityAnimBridgeBaker.Bake(model, clips, request.fps, request.baked_file);
                    response.baked_file = request.baked_file;
                }
                if (request.operation == "preview" && response.status == "ok")
                {
                    if (clips.Count != 1) throw new ArgumentException("A clip is required for preview");
                    AnimBridgePreview.Bake(model, clips[0], request);
                    response.preview_file = request.preview_file;
                }
            }
        }
        catch (Exception error)
        {
            response.status = "error";
            response.reason = error.Message;
            response.errors = new[] { error.ToString() };
            Debug.LogError("AnimBridge desktop helper: " + error);
        }
        Directory.CreateDirectory(Path.GetDirectoryName(output));
        File.WriteAllText(output, JsonUtility.ToJson(response, true));
        Debug.Log("ANIMBRIDGE_DESKTOP_HELPER " + response.status);
    }
}
