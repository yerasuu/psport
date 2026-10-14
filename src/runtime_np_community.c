/* PSN community services offline: NpScore and NpTus/Tss title contexts need the signed-in user, so
 * they are refused; requests are local objects and every query or update fails signed out, as the
 * Score calls in runtime_services.c do. NpUtility's WordFilter and BandwidthTest, and the
 * Matching2 entries runtime_services.c lacks, answer the same way. */
#define _GNU_SOURCE
#include "runtime_np.h"
#include <stdio.h>
#include <string.h>

#define COMMUNITY_INVALID_ARGUMENT ((int32_t)0x80550704)
#define COMMUNITY_INVALID_ID ((int32_t)0x8055070E)
#define COMMUNITY_OUT_OF_MEMORY ((int32_t)0x80550703)
#define BANDWIDTH_TEST_NOT_INITIALIZED ((int32_t)0x80551F02)
#define BANDWIDTH_TEST_CONTEXT_NOT_AVAILABLE ((int32_t)0x80551F07)
#define MATCHING2_CONTEXT_NOT_FOUND ((int32_t)0x80550C06)
#define MATCHING2_INVALID_ARGUMENT ((int32_t)0x80550C0A)

static ABI int32_t title_context_signed_out(uint32_t service_label, int32_t user_id) {
    (void)service_label; (void)user_id;
    return NP_SIGNED_OUT;
}
static ABI int32_t word_filter_title_context(int32_t user_id) {
    (void)user_id;
    return NP_SIGNED_OUT;
}
/* No title context can exist, so there is never one to delete. */
static ABI int32_t delete_title_context(int32_t h_context) {
    (void)h_context;
    return COMMUNITY_INVALID_ID;
}

static int32_t create_request(uint32_t kind, int32_t h_context) {
    if (h_context <= 0) return COMMUNITY_INVALID_ARGUMENT;
    const int64_t h_request = np_object_create(kind);
    return h_request > 0 && h_request <= INT32_MAX ? (int32_t)h_request : COMMUNITY_OUT_OF_MEMORY;
}
static ABI int32_t tus_create_request(int32_t h_context) { return create_request(NP_TUS_REQUEST, h_context); }
static ABI int32_t tus_delete_request(int32_t h_request) {
    return np_object_delete(NP_TUS_REQUEST, h_request) ? 0 : COMMUNITY_INVALID_ID;
}
static ABI int32_t tus_abort_request(int32_t h_request) {
    return np_object_exists(NP_TUS_REQUEST, h_request) ? 0 : COMMUNITY_INVALID_ID;
}
static ABI int32_t word_filter_create_request(int32_t h_context) {
    return create_request(NP_WORD_FILTER_REQUEST, h_context);
}
static ABI int32_t word_filter_delete_request(int32_t h_request) {
    return np_object_delete(NP_WORD_FILTER_REQUEST, h_request) ? 0 : COMMUNITY_INVALID_ID;
}
static ABI int32_t word_filter_abort_request(int32_t h_request) {
    return np_object_exists(NP_WORD_FILTER_REQUEST, h_request) ? 0 : COMMUNITY_INVALID_ID;
}

/* Every query and update of these services is a round trip to the PSN servers. They take more
 * arguments than registers carry, so the call report cannot watch their results: they say so here. */
static int32_t refuse_signed_out(const char *sz_family, int *p_c_reported) {
    if (__atomic_fetch_add(p_c_reported, 1, __ATOMIC_RELAXED) < 4)
        printf("Runtime: %s request refused, signed out -> 0x%08X\n", sz_family, (uint32_t)NP_SIGNED_OUT);
    return NP_SIGNED_OUT;
}
static ABI int32_t score_request_signed_out(void) {
    static int s_c_reported;
    return refuse_signed_out("NpScore", &s_c_reported);
}
static ABI int32_t tus_request_signed_out(void) {
    static int s_c_reported;
    return refuse_signed_out("NpTus", &s_c_reported);
}
static ABI int32_t tss_request_signed_out(void) {
    static int s_c_reported;
    return refuse_signed_out("NpTss", &s_c_reported);
}
static ABI int32_t request_signed_out(int32_t h_request) {
    (void)h_request;
    return NP_SIGNED_OUT;
}

/* A bandwidth test measures against a PSN server; with the cable out none is reachable. */
static ABI int32_t bandwidth_test_start(const void *p_parameter) {
    (void)p_parameter;
    return BANDWIDTH_TEST_CONTEXT_NOT_AVAILABLE;
}
static ABI int32_t bandwidth_test_context(int32_t h_context) {
    (void)h_context;
    return BANDWIDTH_TEST_NOT_INITIALIZED;
}

/* sceNpMatching2CreateContext(A) fails signed out (runtime_services.c), so no context exists. */
static ABI int32_t matching2_create_context(const void *p_parameter, uint16_t *p_out_context_id) {
    if (!p_parameter || !p_out_context_id) return MATCHING2_INVALID_ARGUMENT;
    return NP_SIGNED_OUT;
}
static ABI int32_t matching2_no_context(uint16_t h_context) {
    (void)h_context;
    return MATCHING2_CONTEXT_NOT_FOUND;
}
static ABI int32_t matching2_signed_out(uint16_t h_context) {
    (void)h_context;
    return NP_SIGNED_OUT;
}
/* Matching2's memory statistics: total, current and maximum use; the library holds nothing. */
static ABI int32_t matching2_memory_info(uint64_t *p_out_info) {
    if (!p_out_info) return MATCHING2_INVALID_ARGUMENT;
    memset(p_out_info, 0, 3 * sizeof(*p_out_info));
    return 0;
}
/* Registering a room message callback that no room will ever call. */
static ABI int32_t matching2_register_callback(uint16_t h_context, const void *pfn_callback, void *p_user_argument) {
    (void)h_context; (void)pfn_callback; (void)p_user_argument;
    return 0;
}

static const RuntimeExport s_exports[] = {
    {"sceNpScoreCreateNpTitleCtxA", title_context_signed_out},
    {"sceNpScoreGetFriendsRankingA", score_request_signed_out}, {"sceNpScoreGetFriendsRankingForCrossSave", score_request_signed_out},
    {"sceNpScoreGetGameDataByAccountId", score_request_signed_out},
    {"sceNpScoreGetRankingByAccountId", score_request_signed_out},
    {"sceNpScoreGetRankingByAccountIdForCrossSave", score_request_signed_out},
    {"sceNpScoreGetRankingByAccountIdPcId", score_request_signed_out},
    {"sceNpScoreGetRankingByAccountIdPcIdForCrossSave", score_request_signed_out},
    {"sceNpScoreGetRankingByRangeA", score_request_signed_out}, {"sceNpScoreGetRankingByRangeForCrossSave", score_request_signed_out},
    {"sceNpTssCreateNpTitleCtxA", title_context_signed_out}, {"sceNpTssGetData", tss_request_signed_out},
    {"sceNpTusDeleteNpTitleCtx", delete_title_context},
    {"sceNpTusCreateRequest", tus_create_request}, {"sceNpTusDeleteRequest", tus_delete_request},
    {"sceNpTusAbortRequest", tus_abort_request},
    {"sceNpTusAddAndGetVariableA", tus_request_signed_out}, {"sceNpTusAddAndGetVariableAVUser", tus_request_signed_out},
    {"sceNpTusAddAndGetVariableForCrossSave", tus_request_signed_out},
    {"sceNpTusAddAndGetVariableForCrossSaveVUser", tus_request_signed_out},
    {"sceNpTusDeleteMultiSlotDataA", tus_request_signed_out}, {"sceNpTusDeleteMultiSlotDataVUser", tus_request_signed_out},
    {"sceNpTusGetDataA", tus_request_signed_out}, {"sceNpTusGetDataAVUser", tus_request_signed_out},
    {"sceNpTusGetDataForCrossSave", tus_request_signed_out}, {"sceNpTusGetDataForCrossSaveVUser", tus_request_signed_out},
    {"sceNpTusGetFriendsDataStatusA", tus_request_signed_out},
    {"sceNpTusGetFriendsDataStatusForCrossSave", tus_request_signed_out},
    {"sceNpTusGetFriendsVariableA", tus_request_signed_out}, {"sceNpTusGetFriendsVariableForCrossSave", tus_request_signed_out},
    {"sceNpTusGetMultiSlotVariableA", tus_request_signed_out}, {"sceNpTusGetMultiSlotVariableAVUser", tus_request_signed_out},
    {"sceNpTusGetMultiSlotVariableForCrossSave", tus_request_signed_out},
    {"sceNpTusGetMultiSlotVariableForCrossSaveVUser", tus_request_signed_out},
    {"sceNpTusGetMultiUserDataStatusA", tus_request_signed_out},
    {"sceNpTusGetMultiUserDataStatusAVUser", tus_request_signed_out},
    {"sceNpTusGetMultiUserDataStatusForCrossSave", tus_request_signed_out},
    {"sceNpTusGetMultiUserDataStatusForCrossSaveVUser", tus_request_signed_out},
    {"sceNpTusGetMultiUserVariableA", tus_request_signed_out}, {"sceNpTusGetMultiUserVariableAVUser", tus_request_signed_out},
    {"sceNpTusGetMultiUserVariableForCrossSave", tus_request_signed_out},
    {"sceNpTusGetMultiUserVariableForCrossSaveVUser", tus_request_signed_out},
    {"sceNpTusSetDataA", tus_request_signed_out}, {"sceNpTusSetDataAVUser", tus_request_signed_out},
    {"sceNpTusSetMultiSlotVariableA", tus_request_signed_out}, {"sceNpTusSetMultiSlotVariableVUser", tus_request_signed_out},
    {"sceNpTusTryAndSetVariableA", tus_request_signed_out}, {"sceNpTusTryAndSetVariableAVUser", tus_request_signed_out},
    {"sceNpTusTryAndSetVariableForCrossSave", tus_request_signed_out},
    {"sceNpTusTryAndSetVariableForCrossSaveVUser", tus_request_signed_out},
    {"sceNpWordFilterCreateTitleCtxA", word_filter_title_context}, {"sceNpWordFilterDeleteTitleCtx", delete_title_context},
    {"sceNpWordFilterCreateRequest", word_filter_create_request},
    {"sceNpWordFilterDeleteRequest", word_filter_delete_request},
    {"sceNpWordFilterAbortRequest", word_filter_abort_request}, {"sceNpWordFilterSanitizeComment", request_signed_out},
    {"sceNpWordFilterCensorComment", request_signed_out},
    {"sceNpBandwidthTestInitStart", bandwidth_test_start}, {"sceNpBandwidthTestGetStatus", bandwidth_test_context},
    {"sceNpBandwidthTestShutdown", bandwidth_test_context}, {"sceNpBandwidthTestAbort", bandwidth_test_context},
    {"sceNpMatching2CreateContextA", matching2_create_context},
    {"sceNpMatching2CreateJoinRoomA", matching2_signed_out}, {"sceNpMatching2JoinRoomA", matching2_signed_out},
    {"sceNpMatching2GetRoomDataExternalList", matching2_signed_out},
    {"sceNpMatching2GetRoomDataInternal", matching2_signed_out},
    {"sceNpMatching2GetRoomMemberDataInternal", matching2_signed_out},
    {"sceNpMatching2GetUserInfoListA", matching2_signed_out}, {"sceNpMatching2SetUserInfo", matching2_signed_out},
    {"sceNpMatching2SendRoomMessage", matching2_signed_out}, {"sceNpMatching2SendRoomChatMessage", matching2_signed_out},
    {"sceNpMatching2SignalingGetConnectionInfoA", matching2_signed_out},
    {"sceNpMatching2GetRoomMemberIdListLocal", matching2_no_context},
    {"sceNpMatching2GetRoomPasswordLocal", matching2_no_context},
    {"sceNpMatching2GetSignalingOptParamLocal", matching2_no_context},
    {"sceNpMatching2SetSignalingOptParam", matching2_no_context},
    {"sceNpMatching2GetSslMemoryInfo", matching2_no_context},
    {"sceNpMatching2GetMemoryInfo", matching2_memory_info},
    {"sceNpMatching2RegisterRoomMessageCallback", matching2_register_callback},
};
uintptr_t runtime_np_community_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports, sz_name); }
