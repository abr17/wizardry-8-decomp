#include "wiz8/local_code/Noise.h"

#include "wiz8/3d_code/PList.h"
#include "wiz8/engine_code/GrCycle.h"
#include "wiz8/engine_code/Monster.h"
#include "wiz8/engine_code/Octree.h"
#include "wiz8/engine_code/World.h"
#include "wiz8/layouts/game_status.h"
#include "wiz8/layouts/gameplay_databases.h"
#include "wiz8/local_code/MonsterGroup.h"
#include "wiz8/local_code/MonsterManager.h"
#include "wiz8/startup_world.h"
#include "wiz8/xstatus.h"

#include "random.h"

/* Retail Local Code\Noise.cpp. */

/* The remaining audibility of a noise after distance and the per-hop region
   penalty: positive means the monster group still hears it. */
// FUNCTION: WIZ8 0x004F0E50
int NoiseHearingMargin(int radius, int range, int hops)
{
    return radius + hops * -25000 - range;
}

// FUNCTION: WIZ8 0x004F0E80
void AlertMonsterGroupsToNoise(const srVector3T<float>* position, int radius, int flag)
{
    if (g_status.world_suspended) {
        return;
    }
    srVector3T<float> noise_position = *position;
    for (unsigned int group_index = 0; group_index < PLLength(gXStatus.plsMonsterGroupList);
         ++group_index) {
        W8MonsterGroup* group = GetMonsterGroupByListIndex(group_index);
        W8MonsterInfo* info =
            MonsterInfoFromID(0x2e, "C:\\Projects\\Wizardry 8\\Local Code\\Noise.cpp",
                              group->leader_location_id, true);
        if (info->p3D->deaf) {
            continue;
        }
        if (flag == 1) {
            if (gXStatus.fCombatMode && group->fInCombat) {
                continue;
            }
        } else if (flag == 0 && info->ubDisposition != W8_DISPOSITION_HOSTILE) {
            continue;
        }
        W8MonsterRecord* record = GetMonsterDataForInfo(info);
        srVector3T<float> monster_position = info->p3D->GetPosition();
        /* Retail 0x004F0F49..0x004F0F90: the differences, the (z*z + y*y) + x*x sum and
           the root stay on the x87 stack (53-bit, no float rounding), and _ftol truncates
           to int64 keeping the low 32 bits: a length just below an integer is not rounded
           up to it, and a NaN or out-of-range length (fistp gives 0x8000000000000000)
           counts as 0. The subtraction wraps (found by the w8diff harness). */
        double dx = static_cast<double>(monster_position.x) - noise_position.x;
        double dy = static_cast<double>(monster_position.y) - noise_position.y;
        double dz = static_cast<double>(monster_position.z) - noise_position.z;
        double length = sqrt(dz * dz + dy * dy + dx * dx);
        unsigned int distance = 0;
        if (length > -9223372036854775808.0 && length < 9223372036854775808.0) {
            distance = static_cast<unsigned int>(static_cast<long long>(length));
        }
        int remaining = static_cast<int>(static_cast<unsigned int>(radius) - distance);
        if (remaining <= 0) {
            continue;
        }
        /* Retail 0x004F0F9E..0x004F0FD9: fild remaining * 0.001f + fiadd senses on the
           x87 stack against fild Random; fcompp + test ah,0x41 skips on less, equal or
           unordered (found by the w8diff harness). */
        UINT32 roll = Random(100);
        if (!(static_cast<double>(remaining) * static_cast<double>(g_float_one_thousandth) +
                  static_cast<double>(record->attribute_values[W8_MONSTER_ATTRIBUTE_SENSES]) >
              static_cast<double>(roll))) {
            continue;
        }
        /* Retail 0x004F0EE5: edi keeps this group's own leader info; the leader-group
           redirect below only replaces esi (here 'info'). */
        W8MonsterInfo* group_leader_info = info;
        if (group->leader_group_id != 0) {
            unsigned int leader_index =
                GetMonsterGroupIndexByID(0x54, "C:\\Projects\\Wizardry 8\\Local Code\\Noise.cpp",
                                         group->leader_group_id, true);
            W8MonsterGroup* leader = GetMonsterGroupByListIndex(leader_index);
            info = MonsterInfoFromID(0x55, "C:\\Projects\\Wizardry 8\\Local Code\\Noise.cpp",
                                     leader->leader_location_id, true);
            if (info->p3D->deaf) {
                continue;
            }
        }
        if (flag == 1 && gXStatus.fCombatMode) {
            float range = static_cast<float>(radius);
            int hops;
            if (g_octree->TestNoiseLineOfSight(&monster_position, &noise_position, &range, &hops) ==
                0) {
                continue;
            }
            // Truncate the float move-range to int before scaling, matching retail.
            /* Retail 0x004F1068: push edi (own leader, not the redirected info); fcomp +
               test ah,1 skips on less-than or unordered, so NaN skips too. */
            if (!(static_cast<double>(
                      static_cast<int>(GetMonsterCombatMoveRange(group_leader_info)) * 1000) >=
                  range)) {
                continue;
            }
            /* Retail 0x004F1095: jmp 0x004F10A6 goes straight to MonsterGroupEnterCombat,
               skipping the heard_noise_margin check. */
        } else if (remaining <= info->heard_noise_margin) {
            continue;
        }
        if (flag == 1) {
            MonsterGroupEnterCombat(group);
        }
        info->heard_noise_radius = radius;
        info->heard_noise_margin = remaining;
        info->heard_noise_position = noise_position;
    }
}

// FUNCTION: WIZ8 0x004F1100
void AlertWorldNoise(void)
{
    srVector3T<float> position = g_startup_world->GetPosition();
    AlertMonsterGroupsToNoise(&position, 50000, 1);
}

// FUNCTION: WIZ8 0x004F1150
void AlertCombatNoise(bool large_radius)
{
    if (g_status.world_suspended) {
        return;
    }
    srVector3T<float> position = g_startup_world->GetPosition();
    AlertMonsterGroupsToNoise(&position, large_radius ? 0x927c : 25000, 0);
}
