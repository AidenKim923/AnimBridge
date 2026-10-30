// Evaluates real Unity Humanoid motion and skinned geometry in an isolated project.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using UnityEditor;
using UnityEngine;

public static class AnimBridgePreview
{
    [Serializable] sealed class Bone { public string name, path; public int parent; }
    sealed class PreviewMesh
    {
        public SkinnedMeshRenderer renderer;
        public int[] groups;
        public int count;
        public readonly List<int[]> triangles = new List<int[]>();
        public Mesh baked = new Mesh();
        public Vector3[] source;
        public BoneWeight[] weights;
        public Matrix4x4[] bindposes;
    }

    // World-space vertex from the bones (linear blend skinning). The bones are
    // already correct in meters, so FBX unit scales cannot desync the mesh.
    static Vector3 SkinnedPoint(PreviewMesh mesh, Matrix4x4[] skin, int i)
    {
        var w = mesh.weights[i];
        var v = mesh.source[i];
        return skin[w.boneIndex0].MultiplyPoint3x4(v) * w.weight0 + skin[w.boneIndex1].MultiplyPoint3x4(v) * w.weight1 +
               skin[w.boneIndex2].MultiplyPoint3x4(v) * w.weight2 + skin[w.boneIndex3].MultiplyPoint3x4(v) * w.weight3;
    }

    static List<PreviewMesh> PreviewMeshes(GameObject instance, int limit)
    {
        var renderers = instance.GetComponentsInChildren<SkinnedMeshRenderer>(true)
            .Where(renderer => renderer.sharedMesh != null && renderer.sharedMesh.vertexCount > 0).ToArray();
        int total = renderers.Sum(renderer => renderer.sharedMesh.vertexCount);
        var result = new List<PreviewMesh>();
        foreach (var renderer in renderers)
        {
            var source = renderer.sharedMesh;
            var vertices = source.vertices;
            int budget = Math.Max(8, (int)((long)limit * vertices.Length / Math.Max(total, 1)));
            float step = Math.Max(source.bounds.size.magnitude / 160f, .000001f);
            var mesh = new PreviewMesh { renderer = renderer, groups = new int[vertices.Length], source = vertices,
                weights = source.boneWeights, bindposes = source.bindposes };
            // Cluster nearby vertices, retaining connected triangles rather than
            // dropping faces and leaving holes throughout the mannequin.
            for (int attempt = 0; attempt < 60; attempt++, step *= 1.3f)
            {
                var cells = new Dictionary<Vector3Int, int>();
                for (int i = 0; i < vertices.Length; i++)
                {
                    var point = vertices[i];
                    var cell = new Vector3Int(Mathf.RoundToInt(point.x / step), Mathf.RoundToInt(point.y / step), Mathf.RoundToInt(point.z / step));
                    if (!cells.TryGetValue(cell, out int group)) { group = cells.Count; cells.Add(cell, group); }
                    mesh.groups[i] = group;
                }
                mesh.count = cells.Count;
                if (mesh.count <= budget) break;
            }
            var triangles = source.triangles;
            var seen = new HashSet<string>();
            for (int i = 0; i < triangles.Length; i += 3)
            {
                int a = mesh.groups[triangles[i]], b = mesh.groups[triangles[i + 1]], c = mesh.groups[triangles[i + 2]];
                if (a == b || b == c || a == c) continue;
                if (seen.Add(a + ":" + b + ":" + c)) mesh.triangles.Add(new[] { a, b, c });
            }
            renderer.updateWhenOffscreen = true;
            result.Add(mesh);
        }
        return result;
    }

    static void AppendFrames(StringBuilder text, List<float[][]> frames)
    {
        text.Append('[');
        for (int frame = 0; frame < frames.Count; frame++)
        {
            if (frame > 0) text.Append(',');
            text.Append('[');
            for (int point = 0; point < frames[frame].Length; point++)
            {
                if (point > 0) text.Append(',');
                text.Append('[');
                for (int axis = 0; axis < 3; axis++)
                {
                    if (axis > 0) text.Append(',');
                    float value = frames[frame][point][axis];
                    if (!float.IsFinite(value)) throw new InvalidOperationException("Preview contains a non-finite position");
                    text.Append(value.ToString("R", CultureInfo.InvariantCulture));
                }
                text.Append(']');
            }
            text.Append(']');
        }
        text.Append(']');
    }

    static float[] Point(Vector3 point) => new[] { -point.x, point.y, point.z };

    public static void Bake(GameObject model, AnimationClip clip, AnimBridgeDesktopHelper.Request request)
    {
        if (AnimationMode.InAnimationMode()) throw new InvalidOperationException("AnimationMode is already in use");
        var instance = UnityEngine.Object.Instantiate(model);
        instance.hideFlags = HideFlags.HideAndDontSave;
        var meshes = new List<PreviewMesh>();
        try
        {
            var transforms = instance.GetComponentsInChildren<Transform>(true);
            var indices = transforms.Select((transform, index) => (transform, index)).ToDictionary(pair => pair.transform, pair => pair.index);
            if (request.include_mesh) meshes = PreviewMeshes(instance, Mathf.Clamp(request.max_mesh_vertices, 64, 5000));
            float duration = Mathf.Max(0, clip.length);
            float fps = request.fps > 0 ? request.fps : clip.frameRate > 0 ? clip.frameRate : 30f;
            int count = duration > 0 ? Math.Max(2, Mathf.RoundToInt(duration * fps) + 1) : 1;
            int requestedCount = count;
            int points = transforms.Length + meshes.Sum(mesh => mesh.count);
            int budget = Math.Max(duration > 0 ? 2 : 1, Mathf.Clamp(request.max_point_frames, 1000, 500000) / Math.Max(points, 1));
            count = Math.Min(count, Math.Min(Mathf.Clamp(request.max_frames, 2, 1000), budget));
            fps = duration > 0 ? (count - 1) / duration : fps;
            var frames = new List<float[][]>(count);
            var meshFrames = new List<float[][]>(count);
            AnimationMode.StartAnimationMode();
            try
            {
                for (int frame = 0; frame < count; frame++)
                {
                    float time = count > 1 ? duration * frame / (count - 1) : 0;
                    AnimationMode.BeginSampling();
                    try { AnimationMode.SampleAnimationClip(instance, clip, time); }
                    finally { AnimationMode.EndSampling(); }
                    frames.Add(transforms.Select(transform => Point(transform.position)).ToArray());
                    if (meshes.Count > 0)
                    {
                        var values = new List<float[]>();
                        foreach (var mesh in meshes)
                        {
                            var bones = mesh.renderer.bones;
                            bool skinned = bones.Length > 0 && mesh.bindposes.Length == bones.Length && mesh.weights.Length == mesh.source.Length;
                            Matrix4x4[] skin = null;
                            if (skinned)
                            {
                                skin = new Matrix4x4[bones.Length];
                                for (int b = 0; b < bones.Length; b++)
                                    skin[b] = bones[b] != null ? bones[b].localToWorldMatrix * mesh.bindposes[b] : mesh.renderer.transform.localToWorldMatrix;
                            }
                            var toWorld = mesh.renderer.transform.localToWorldMatrix;
                            var sums = new Vector3[mesh.count];
                            var counts = new int[mesh.count];
                            for (int i = 0; i < mesh.source.Length; i++)
                            {
                                int group = mesh.groups[i];
                                sums[group] += skinned ? SkinnedPoint(mesh, skin, i) : toWorld.MultiplyPoint3x4(mesh.source[i]);
                                counts[group]++;
                            }
                            values.AddRange(sums.Select((sum, index) => Point(sum / Math.Max(counts[index], 1))));
                        }
                        meshFrames.Add(values.ToArray());
                    }
                }
            }
            finally { AnimationMode.StopAnimationMode(); }
            var text = new StringBuilder();
            text.Append("{\"schema_version\":1,\"status\":\"ready\",\"kind\":\"bone-motion\",\"coordinate_system\":\"right-handed-y-up-meters\",");
            text.Append("\"fps\":").Append(fps.ToString("R", CultureInfo.InvariantCulture));
            text.Append(",\"duration\":").Append(duration.ToString("R", CultureInfo.InvariantCulture));
            text.Append(",\"frame_count\":").Append(count).Append(",\"bones\":[");
            for (int i = 0; i < transforms.Length; i++)
            {
                if (i > 0) text.Append(',');
                var transform = transforms[i];
                text.Append(JsonUtility.ToJson(new Bone { name = transform.name,
                    path = AnimationUtility.CalculateTransformPath(transform, instance.transform),
                    parent = transform.parent != null && indices.TryGetValue(transform.parent, out int parent) ? parent : -1 }));
            }
            text.Append("],\"frames\":"); AppendFrames(text, frames);
            if (meshFrames.Count > 0)
            {
                text.Append(",\"mesh\":{\"kind\":\"evaluated-skinned-mesh\",\"triangles\":[");
                int offset = 0, face = 0;
                foreach (var mesh in meshes)
                {
                    foreach (var triangle in mesh.triangles)
                    {
                        if (face++ > 0) text.Append(',');
                        text.Append('[').Append(triangle[0] + offset).Append(',').Append(triangle[1] + offset).Append(',').Append(triangle[2] + offset).Append(']');
                    }
                    offset += mesh.count;
                }
                text.Append("],\"frames\":"); AppendFrames(text, meshFrames); text.Append('}');
            }
            text.Append(",\"warnings\":[");
            if (count < requestedCount) text.Append("\"Preview sample rate reduced to fit payload budget\"");
            text.Append("]}");
            Directory.CreateDirectory(Path.GetDirectoryName(request.preview_file));
            File.WriteAllText(request.preview_file, text.ToString(), new UTF8Encoding(false));
        }
        finally
        {
            foreach (var mesh in meshes) UnityEngine.Object.DestroyImmediate(mesh.baked);
            UnityEngine.Object.DestroyImmediate(instance);
        }
    }
}
