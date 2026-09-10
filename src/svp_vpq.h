// ---------------------------------------------------------------------------
// Stride Value Predictor (SVP) + Value Prediction Queue (VPQ)
//
// EXCERPT. In the original source this code lives inside `class renamer`,
// which also contains the register-renaming implementation (RMT, AMT,
// physical register file, free list, active list, branch checkpoints). That
// portion is a separate graded assignment and is deliberately omitted here.
//
// Only the value-prediction members and methods appear below. They are
// verbatim apart from normalised indentation. This header does not compile
// standalone: it depends on the surrounding out-of-order framework for
// `parameters.h`, for the pipeline stages that call these methods, and for
// the `renamer` class that owns them.
//
// See the README for how the pieces fit together and where each method is
// called from.
// ---------------------------------------------------------------------------

#include <inttypes.h>
#include <cassert>
#include <vector>

using namespace std;

// Supplied by the framework's parameters.h. Gates the selective filter so
// that the default build reproduces the specified design exactly.
extern bool RESEARCH_ENABLE;

// ---------------------------------------------------------------------------
// SVP entry. The first five fields are the specified predictor state; the
// sixth is my addition.
// ---------------------------------------------------------------------------
struct svp_t
{
    uint64_t valid = 0;
    uint64_t tag;
    uint64_t confidence = 0;
    int64_t  retired_value;
    int64_t  stride = 0;
    uint64_t instance = 0;

    // Selective-recovery filter (research mode only).
    //  - armed by svp_record_squash() when execute detects a confident value
    //    misprediction (PENALTY_INIT = 64 here, found best by trial-n-error)
    //  - bleeds down by 1 inside svp_pred_confident, so the PC gets filtered
    //    out for ~64 next prediction checks. helps avoiding back-to-back
    //    inflight wrong predictions for same bad PC, untill retire-time
    //    training has a chance to reset its confidence.
    //  - costs only 1 byte per svp entry (uint8_t)
    uint8_t penalty = 0;
};

// ---------------------------------------------------------------------------
// VPQ entry. Carries a prediction through the pipeline in program order so
// that retire can train the predictor from the computed value.
// ---------------------------------------------------------------------------
struct vpq_t
{
    uint64_t pc;
    int64_t  value;
    bool     valid;
    bool     value_valid;
    bool     incremented_instance;
};

// ---------------------------------------------------------------------------
// Value-prediction state and methods, as they appear inside class renamer.
// ---------------------------------------------------------------------------

    vector<svp_t> svp;
    vector<vpq_t> vpq;

    uint64_t     svp_size;            // number of SVP entries
    uint64_t     svp_index_bits;      // log2(svp_size)
    uint64_t     svp_tag_bits;        // 0 = untagged, else partial/full tag
    unsigned int svp_confidence_max;
    uint64_t     vpq_size;
    uint64_t     vpq_head;            // oldest in-flight prediction
    uint64_t     vpq_tail;            // newest in-flight prediction
    uint64_t     vpq_count;
    bool         vpq_head_phase;      // phase bits distinguish EMPTY from FULL
    bool         vpq_tail_phase;

// Constructor fragment (the renamer's own initialisation is omitted):
//
//     svp.resize(svp_size);
//     vpq.resize(vpq_size);
//     vpq_head = 0;  vpq_tail = 0;  vpq_count = 0;
//     vpq_head_phase = false;  vpq_tail_phase = false;

// --- lookup ---------------------------------------------------------------

bool svp_anveshana(uint64_t pc, unsigned int &svp_id, bool &hit)
{
    svp_id = (unsigned int)((pc >> 2) % svp_size);

    if (!svp[svp_id].valid) {   // If the table is empty, it is a miss
        hit = false;
        return true;
    }

    if (svp_tag_bits == 0) {    // Any entry is marked as HIT if there is no tag
        hit = true;
        return true;
    }

    // Extracting TAG bits from the PC
    uint64_t tag_mask = (svp_tag_bits >= 64) ? ~0ULL : ((1ULL << svp_tag_bits) - 1ULL);
    uint64_t pc_tag   = (pc >> (2 + svp_index_bits)) & tag_mask;  // past index bits

    hit = (svp[svp_id].tag == pc_tag);
    return true;
}

// --- allocation -----------------------------------------------------------

// Pre-allocate an SVP entry at rename time when no valid entry exists for
// this PC, so every eligible instruction reaching rename has an entry.
void svp_ketayimpu(uint64_t pc)
{
    unsigned int svp_id = (unsigned int)((pc >> 2) % svp_size);

    if (svp_tag_bits == 0) {
        svp[svp_id].tag = 0;
    } else {
        uint64_t tag_mask = (svp_tag_bits >= 64) ? ~0ULL : ((1ULL << svp_tag_bits) - 1ULL);
        svp[svp_id].tag = (pc >> (2 + svp_index_bits)) & tag_mask;
    }

    svp[svp_id].valid         = 1;
    svp[svp_id].retired_value = 0;
    svp[svp_id].stride        = 0;
    svp[svp_id].confidence    = 0;
    svp[svp_id].instance      = 0;
    svp[svp_id].penalty       = 0;   // reset filter on fresh alloc
}

bool svp_first_ketayimpu(uint64_t pc)
{
    unsigned int svp_id = (unsigned int)((pc >> 2) % svp_size);

    if (svp[svp_id].valid)
        return false;

    svp_ketayimpu(pc);
    return true;
}

// --- prediction -----------------------------------------------------------

// Called after svp_instance_plus_1(), so `instance` already counts this
// instruction. See README on why this is (instance + 1) * stride in effect.
int64_t svp_prediction_theesko(unsigned int svp_id)
{
    assert(svp_id < svp_size);
    return (svp[svp_id].retired_value + (svp[svp_id].instance * svp[svp_id].stride));
}

// instance++ to compute the pred value
void svp_instance_plus_1(unsigned int svp_id)
{
    assert(svp_id < svp_size);
    svp[svp_id].instance++;
}

// check confidence of prediction
bool svp_pred_confident(unsigned int svp_id)
{
    assert(svp_id < svp_size);

    // Research-only path: a PC that recently caused a value squash is
    // filtered for the next few prediction checks. This stops the "inflight
    // storm" where multiple young instances of the same bad PC each generate
    // confident wrong predictions before the first one even retires and
    // resets confidence. The penalty bleeds down to 0 by itself, so the PC
    // heals automatically if it stops squashing. Gated under RESEARCH_ENABLE
    // so the default run is unchanged.
    if (RESEARCH_ENABLE) {
        if (svp[svp_id].penalty > 0) {
            svp[svp_id].penalty--;
            return false;
        }
    }
    return (svp[svp_id].confidence == svp_confidence_max);
}

// Called from execute.cc whenever a confident value misprediction is
// detected (used && !correct). Arms the per-entry penalty so future
// predictions for this PC are gated until the squash resolves and confidence
// retrains. Idempotent and tag-safe. No-op outside research mode.
void svp_record_squash(uint64_t pc)
{
    if (!RESEARCH_ENABLE) return;   // default path - do nothing

    unsigned int svp_id = 0;
    bool hit = false;
    svp_anveshana(pc, svp_id, hit);
    if (hit) {
        svp[svp_id].penalty = 64;   // ~64 future prediction checks gated
    }
}

// --- VPQ ------------------------------------------------------------------

bool vpq_nindindha(uint64_t i)   // check if vpq is full ?
{
    return ((vpq_count + i) > vpq_size);
}

unsigned int vpq_ketayimpu(uint64_t pc)
{
    assert(vpq_count < vpq_size);

    unsigned int index = (unsigned int)vpq_tail;

    vpq[index].valid                = 1;
    vpq[index].pc                   = pc;
    vpq[index].value                = 0;
    vpq[index].value_valid          = 0;
    vpq[index].incremented_instance = 0;

    if (vpq_tail == vpq_size - 1) {
        vpq_tail = 0;
        vpq_tail_phase = !vpq_tail_phase;
    } else {
        vpq_tail++;
    }
    vpq_count++;

    return index;
}

// load vpq entry with value
void vpq_write_value(unsigned int vpq_idx, int64_t val)
{
    assert(vpq_idx < vpq_size);
    assert(vpq[vpq_idx].valid);
    vpq[vpq_idx].value       = val;
    vpq[vpq_idx].value_valid = 1;
}

void vpq_instance_set(unsigned int vpq_index, unsigned int svp_idx)
{
    assert(vpq_index < vpq_size);
    assert(vpq[vpq_index].valid);
    vpq[vpq_index].incremented_instance = 1;
}

// vpq head instr valid return
bool vpq_head_rdy()
{
    if (vpq_count == 0)
        return false;

    return (vpq[vpq_head].valid && vpq[vpq_head].value_valid);
}

// vpq head pc return
uint64_t vpq_head_pc()
{
    assert(vpq_count > 0);
    assert(vpq[vpq_head].valid);
    return vpq[vpq_head].pc;
}

// vpq head instr return
int64_t vpq_head_val()
{
    assert(vpq_count > 0);
    assert(vpq[vpq_head].valid);
    assert(vpq[vpq_head].value_valid);
    return vpq[vpq_head].value;
}

// Delete the head after SVP has done training
void vpq_pop()
{
    assert(vpq_count > 0);
    assert(vpq[vpq_head].valid);
    vpq[vpq_head].valid       = 0;
    vpq[vpq_head].pc          = 0;
    vpq[vpq_head].value       = 0;
    vpq[vpq_head].value_valid = 0;

    // Phase bit handling
    if (vpq_head == vpq_size - 1) {
        vpq_head = 0;
        vpq_head_phase = !vpq_head_phase;
    } else {
        vpq_head++;
    }

    vpq_count--;
}

// --- training (retire) ----------------------------------------------------

// Trains from the *computed* value, never from the prediction. Called in
// program order as the VPQ head is popped at retire.
void svp_seekshana(uint64_t pc, int64_t value)
{
    unsigned int svp_id = 0;
    bool hit = false;

    svp_anveshana(pc, svp_id, hit);

    if (hit) {
        int64_t new_strd = value - (int64_t)svp[svp_id].retired_value;

        if (new_strd == svp[svp_id].stride) {
            if ((uint64_t)svp[svp_id].confidence < svp_confidence_max)
                svp[svp_id].confidence++;
        }
        else {
            svp[svp_id].confidence = 0;
        }

        svp[svp_id].stride        = new_strd;
        svp[svp_id].retired_value = value;

        if (svp[svp_id].instance > 0)
            svp[svp_id].instance--;
    }
    else {
        svp_id = (unsigned int)((pc >> 2) % svp_size);
        svp[svp_id].valid = 1;

        if (svp_tag_bits == 0) {
            svp[svp_id].tag = 0;
        }
        else {
            uint64_t tag_mask = (svp_tag_bits >= 64) ? ~0ULL : ((1ULL << svp_tag_bits) - 1ULL);
            svp[svp_id].tag = (pc >> (2 + svp_index_bits)) & tag_mask;
        }

        // new entry: seed retired_value and stride (per spec: both = value)
        svp[svp_id].retired_value = value;
        svp[svp_id].stride        = (int64_t)value;
        svp[svp_id].confidence    = 0;

        // Count younger in-flight instances of this PC already in the VPQ,
        // so the fresh entry's instance counter reflects reality.
        uint64_t inst_count = 0;
        if (vpq_count > 1) {
            uint64_t walk_idx   = (vpq_head + 1) % vpq_size;
            uint64_t walk_count = vpq_count - 1;
            for (uint64_t w = 0; w < walk_count; w++) {
                if (vpq[walk_idx].valid && vpq[walk_idx].pc == pc)
                    inst_count++;
                walk_idx = (walk_idx + 1) % vpq_size;
            }
        }
        svp[svp_id].instance = inst_count;
    }
}
