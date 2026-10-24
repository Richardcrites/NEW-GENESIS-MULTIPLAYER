#pragma once

#include <cstdint>
#include <cmath>

namespace RE { class Actor; class NiAVObject; class NiNode; class NiMatrix3; }

namespace Pulsar::BoneSync
{
    struct Quat { float x, y, z, w; };

    static constexpr int NUM_BONES = 19;
    inline const char* BONE_NAMES[NUM_BONES] = {
        "C_Hips", "C_Spine", "C_Spine1", "C_Spine2", "C_Chest",
        "C_Neck", "C_Head",
        "L_Thigh", "L_Calf", "L_Foot",
        "R_Thigh", "R_Calf", "R_Foot",
        "L_Clavicle", "L_Biceps", "L_Forearm",
        "R_Clavicle", "R_Biceps", "R_Forearm"
    };

    struct BoneCache
    {
        RE::NiAVObject* bones[NUM_BONES] = {};
        bool resolved = false;
        void Resolve(RE::NiNode* root);
    };

    struct BoneFrame
    {
        Quat rotations[NUM_BONES];
        float hipsPosX = 0.0f, hipsPosY = 0.0f, hipsPosZ = 0.0f;
    };

    Quat MatrixToQuat(const RE::NiMatrix3& m);
    void QuatToMatrix(const Quat& q, RE::NiMatrix3& m);

    bool ReadBones(RE::Actor* actor, BoneCache& cache, BoneFrame& frame);
    void WriteBones(RE::Actor* actor, BoneCache& cache, const BoneFrame& frame);
}
