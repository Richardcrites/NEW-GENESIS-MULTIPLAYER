#include "PCH.h"
#include "Animation/BoneSync.h"

#include <RE/A/Actor.h>
#include <RE/B/BSFixedString.h>
#include <RE/N/NiAVObject.h>
#include <RE/N/NiNode.h>
#include <RE/N/NiMatrix3.h>
#include <RE/N/NiPoint.h>
#include <RE/N/NiTransform.h>
#include <RE/T/TESObjectREFR.h>
#include "NASLog.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

using namespace std::string_view_literals;

namespace Pulsar::BoneSync
{
    // --- NiMatrix3 (3x3 rotation from 3x NiPoint4) -> Quaternion ---
    Quat MatrixToQuat(const RE::NiMatrix3& m)
    {
        // m.entry[row][col]: entry[0] = row0, entry[1] = row1, entry[2] = row2
        // NiPoint4 has x,y,z,w - we use x,y,z as columns
        float m00 = m.entry[0][0], m01 = m.entry[0][1], m02 = m.entry[0][2];
        float m10 = m.entry[1][0], m11 = m.entry[1][1], m12 = m.entry[1][2];
        float m20 = m.entry[2][0], m21 = m.entry[2][1], m22 = m.entry[2][2];

        Quat q;
        float trace = m00 + m11 + m22;
        if (trace > 0.0f) {
            float s = std::sqrt(trace + 1.0f) * 2.0f;
            q.w = 0.25f * s;
            q.x = (m21 - m12) / s;
            q.y = (m02 - m20) / s;
            q.z = (m10 - m01) / s;
        } else if (m00 > m11 && m00 > m22) {
            float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
            q.w = (m21 - m12) / s;
            q.x = 0.25f * s;
            q.y = (m01 + m10) / s;
            q.z = (m02 + m20) / s;
        } else if (m11 > m22) {
            float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
            q.w = (m02 - m20) / s;
            q.x = (m01 + m10) / s;
            q.y = 0.25f * s;
            q.z = (m12 + m21) / s;
        } else {
            float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
            q.w = (m10 - m01) / s;
            q.x = (m02 + m20) / s;
            q.y = (m12 + m21) / s;
            q.z = 0.25f * s;
        }
        return q;
    }

    // --- Quaternion -> NiMatrix3 ---
    void QuatToMatrix(const Quat& q, RE::NiMatrix3& m)
    {
        float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

        m.entry[0][0] = 1.0f - 2.0f * (yy + zz);
        m.entry[0][1] = 2.0f * (xy - wz);
        m.entry[0][2] = 2.0f * (xz + wy);

        m.entry[1][0] = 2.0f * (xy + wz);
        m.entry[1][1] = 1.0f - 2.0f * (xx + zz);
        m.entry[1][2] = 2.0f * (yz - wx);

        m.entry[2][0] = 2.0f * (xz - wy);
        m.entry[2][1] = 2.0f * (yz + wx);
        m.entry[2][2] = 1.0f - 2.0f * (xx + yy);
    }

    // --- Raw NiNode tree walk (no virtual calls, just offsets) ---
    // NiObjectNET+0x10 = BSFixedString name
    // NiNode: children BSTArray starts at +0x130
    //   BSTArray layout: +0x00 = T* data (NiPointer<NiAVObject>*), +0x08 = uint32 size

    static RE::NiAVObject* RawFindBone(RE::NiAVObject* node, const char* targetName, int depth = 0)
    {
        if (!node || depth > 10) return nullptr;

        // Check name at NiObjectNET+0x10
        auto* namePtr = reinterpret_cast<RE::BSFixedString*>(reinterpret_cast<uint8_t*>(node) + 0x10);
        const char* nodeName = namePtr->c_str();
        if (nodeName && strcmp(nodeName, targetName) == 0) {
            return node;
        }

        // Children: data ptr at +0x138, count as uint16 at +0x142
        // (confirmed: prior code read uint16 at +0x142 and found 11 children)
        auto* nodeBytes = reinterpret_cast<uint8_t*>(node);
        auto** childDataPtr = *reinterpret_cast<RE::NiAVObject***>(nodeBytes + 0x138);
        uint16_t childCount = *reinterpret_cast<uint16_t*>(nodeBytes + 0x142);

        if (!childDataPtr || childCount == 0 || childCount > 200) return nullptr;

        for (uint32_t i = 0; i < childCount; i++) {
            auto* child = *reinterpret_cast<RE::NiAVObject**>(
                reinterpret_cast<uint8_t*>(childDataPtr) + i * sizeof(void*));
            if (!child) continue;

            auto* result = RawFindBone(child, targetName, depth + 1);
            if (result) return result;
        }

        return nullptr;
    }

    // --- Resolve bone pointers by name (raw walk, no virtuals) ---
    void BoneCache::Resolve(RE::NiNode* root)
    {
        if (!root) return;

        // Diagnostic: dump root name and first child names
        auto* rootName = reinterpret_cast<RE::BSFixedString*>(reinterpret_cast<uint8_t*>(root) + 0x10);
        PLOG_INFO("[PULSAR] BoneResolve: root name='{}'"sv,
                  rootName->c_str() ? rootName->c_str() : "(null)");

        auto* rootBytes = reinterpret_cast<uint8_t*>(root);
        auto** childDataPtr = *reinterpret_cast<RE::NiAVObject***>(rootBytes + 0x138);
        uint16_t childCount = *reinterpret_cast<uint16_t*>(rootBytes + 0x142);
        PLOG_INFO("[PULSAR] BoneResolve: children={} dataPtr={}"sv, childCount, (void*)childDataPtr);

        if (childDataPtr && childCount > 0 && childCount < 50) {
            for (uint32_t i = 0; i < childCount && i < 5; i++) {
                auto* child = *reinterpret_cast<RE::NiAVObject**>(
                    reinterpret_cast<uint8_t*>(childDataPtr) + i * sizeof(void*));
                if (child) {
                    auto* childName = reinterpret_cast<RE::BSFixedString*>(
                        reinterpret_cast<uint8_t*>(child) + 0x10);
                    PLOG_INFO("[PULSAR]   child[{}] name='{}'"sv, i,
                              childName->c_str() ? childName->c_str() : "(null)");
                }
            }
        }

        int found = 0;
        for (int i = 0; i < NUM_BONES; i++) {
            bones[i] = RawFindBone(reinterpret_cast<RE::NiAVObject*>(root), BONE_NAMES[i]);
            if (bones[i]) found++;
        }
        resolved = true;
        PLOG_INFO("[PULSAR] BoneCache: resolved {}/{} bones"sv, found, NUM_BONES);
    }

    // --- SEH wrapper for bone read ---
    static int SafeReadBone(RE::NiAVObject* bone, Quat* outRot, float* outPos)
    {
        __try {
            *outRot = MatrixToQuat(bone->local.rotate);
            if (outPos) {
                outPos[0] = bone->local.translate.x;
                outPos[1] = bone->local.translate.y;
                outPos[2] = bone->local.translate.z;
            }
            return 1;
        } __except(1) {
            return 0;
        }
    }

    // --- SEH wrapper for bone write ---
    static int SafeWriteBone(RE::NiAVObject* bone, const Quat* rot, const float* pos)
    {
        __try {
            if (rot) {
                RE::NiMatrix3 mat;
                QuatToMatrix(*rot, mat);
                bone->local.rotate = mat;
            }
            if (pos) {
                bone->local.translate.x = pos[0];
                bone->local.translate.y = pos[1];
                bone->local.translate.z = pos[2];
            }
            return 1;
        } __except(1) {
            return 0;
        }
    }

    // --- SEH-safe get root NiNode from actor ---
    static RE::NiNode* SafeGetRootNode(RE::Actor* actor)
    {
        __try {
            auto* loadedGuard = reinterpret_cast<uint8_t*>(actor) + 0xB8;
            auto* loadedData = *reinterpret_cast<RE::LOADED_REF_DATA**>(loadedGuard);
            if (!loadedData || !loadedData->data3D) return nullptr;
            return loadedData->data3D->GetAsNiNode();
        } __except(1) {
            return nullptr;
        }
    }

    // --- Read all bone transforms from actor ---
    bool ReadBones(RE::Actor* actor, BoneCache& cache, BoneFrame& frame)
    {
        if (!actor) return false;

        static bool s_readLog = false;
        if (!s_readLog) { s_readLog = true; PLOG_INFO("[PULSAR] ReadBones: entering..."sv); }

        auto* root = SafeGetRootNode(actor);
        if (!root) return false;

        static bool s_rootLog = false;
        if (!s_rootLog) { s_rootLog = true; PLOG_INFO("[PULSAR] ReadBones: root={}"sv, (void*)root); }

        if (!cache.resolved) cache.Resolve(root);

        for (int i = 0; i < NUM_BONES; i++) {
            if (cache.bones[i]) {
                float* posPtr = (i == 0) ? &frame.hipsPosX : nullptr;
                SafeReadBone(cache.bones[i], &frame.rotations[i], posPtr);
            }
        }
        return true;
    }

    // --- Write bone transforms to actor ---
    void WriteBones(RE::Actor* actor, BoneCache& cache, const BoneFrame& frame)
    {
        if (!actor) return;

        if (!cache.resolved) {
            auto* root = SafeGetRootNode(actor);
            if (!root) return;
            cache.Resolve(root);
        }

        for (int i = 0; i < NUM_BONES; i++) {
            if (!cache.bones[i]) continue;
            if (i == 0) {
                // Hips: apply rotation WITH yaw stripped out.
                // The yaw (Z-axis rotation) is already handled by data.angle.z.
                // We only want the animation-driven pitch/roll (body lean/tilt).
                Quat hipsQ = frame.rotations[0];
                // Extract yaw from quaternion: zero out the yaw component
                // For Z-up coordinate system: yaw = atan2(2(wz+xy), 1-2(yy+zz))
                // Simplification: project quaternion to remove Z rotation
                // q_no_yaw = normalize(w, x, y, 0) - removes Z-axis rotation
                float len = std::sqrt(hipsQ.w * hipsQ.w + hipsQ.x * hipsQ.x + hipsQ.y * hipsQ.y);
                if (len > 0.001f) {
                    hipsQ.w /= len;
                    hipsQ.x /= len;
                    hipsQ.y /= len;
                    hipsQ.z = 0.0f;
                }
                SafeWriteBone(cache.bones[i], &hipsQ, &frame.hipsPosX);
            } else {
                SafeWriteBone(cache.bones[i], &frame.rotations[i], nullptr);
            }
        }
    }
}
