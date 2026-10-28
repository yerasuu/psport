// SPDX-License-Identifier: GPL-2.0-or-later
/* NID -> function name and the result an import gives only because the console has no connection
 * (psport runs as an unplugged, signed-out console), so the call report lists it as `offline`
 * instead of `failed` or `stub`: still counted and named, never hidden. A result the status table
 * (import_statuses.inc) already claims stays a `status`. By import and result, never by title.
 * Result 0 is for an import answered by the runtime's stub that does nothing.
 *   0x80410133 (SCE_NET_ERROR_ENETUNREACH): runtime_services.c net_unreachable (Epoll, Resolver
 *     StartNtoa/StartAton: every lookup fails at once offline) and runtime_socket.c (sceNetConnect,
 *     sceNetSendto refuse every peer but loopback). Seen in real logs: sceNetSendto,
 *     sceNetResolverStartNtoa, sceNetEpollWait.
 *   errno 51 (ENETUNREACH): the POSIX connect, sendto and sendmsg of runtime_socket.c (do_connect,
 *     do_sendto, do_sendmsg) on a non-loopback peer. Not yet seen in a log.
 *   0x80550006 (SCE_NP_ERROR_SIGNED_OUT): the NP calls runtime_services.c answers np_signed_out,
 *     np_account_id, np_account_id_online or np_account_info, since no PSN account is signed in.
 *     Seen in real logs: sceNpGetAccountIdA, sceNpAuthGetIdTokenA, sceNpGetOnlineId.
 *   0: sceNpSetNpTitleId, sceNpNotifyPlusFeature, sceNpSetContentRestriction, sceNpAbortRequest and
 *     sceNpMatching2Initialize only talk to PSN, sceHttpSetNonblock, sceHttpsEnableOption and
 *     sceHttpAddRequestHeader only prepare a request no server will see; all are runtime_stub
 *     (src/system/runtime_services.c, src/net/runtime_http.c) and appear as stubs in real logs.
 * Sorted by NID in strcmp order, then by result, for a binary search. Kept by hand. */
{"0UvTFeomAUM","sceNpSignalingActivateConnection",0x80550006},
{"10t3e5+JPnU","sceNpMatching2Initialize",0},
{"1n-dGukBgnY","sceNpTusCreateNpTitleCtxA",0x80550006},
{"2b3TI0mDYiI","sceNpScoreCensorComment",0x80550006},
{"2rsFmlGWleQ","sceNpCheckNpAvailability",0x80550006},
{"5yYjEdd4t8Y","sceNpSignalingCreateContext",0x80550006},
{"7vjNQ6Z1op0","sceNpMatching2ContextStart",0x80550006},
{"8VBTeRf1ZwI","sceNpGetAccountDateOfBirth",0x80550006},
{"A2CQ3kgSopQ","sceNpSetContentRestriction",0},
{"AUVfU6byg3c","sceNpMatching2KickoutRoomMember",0x80550006},
{"Apb4YDxKsRI","sceNetResolverStartAton",0x80410133},
{"BBbJ92uUdCg","sceNpMatching2LeaveLobby",0x80550006},
{"BD6kfx442Do","sceNpMatching2LeaveRoom",0x80550006},
{"CSIMDsVjs-g","sceNpMatching2JoinRoom",0x80550006},
{"CocbHVIKPE8","sceNpAuthGetIdTokenA",0x80550006},
{"ETS-uM-vH9Q","sceNpScoreGetRankingByNpIdPcId",0x80550006},
{"EY28T2bkN7k","sceHttpAddRequestHeader",0},
{"Ec63y59l9tw","sceNpSetNpTitleId",0},
{"Gaxrp3EWY-M","sceNpNotifyPlusFeature",0},
{"Ghz9iWDUtC4","sceNpGetAccountCountry",0x80550006},
{"HoqTrkS9c5Q","sceNpMatching2SetRoomMemberDataInternal",0x80550006},
{"IPb1hd1wAGc","sceNpGetGamePresenceStatus",0x80550006},
{"KBHxDjyk-jA","sceNpScoreGetRankingByRange",0x80550006},
{"KxGkOrQJTqY","sceNpAuthGetAuthorizationCode",0x80550006},
{"LhCPctIICxQ","sceNpMatching2GetServerId",0x80550006},
{"LoVMVrijVOk","sceNpScoreGetBoardInfo",0x80550006},
{"NCP3bLGPt+o","sceNpMatching2GrantRoomOwner",0x80550006},
{"Nd91WaWmG2w","sceNetResolverStartNtoa",0x80410133},
{"OXXX4mUk3uk","sceNetConnect",0x80410133},
{"OzKvTvg3ZYU","sceNpAbortRequest",0},
{"S9D8JSYIrjE","sceNpMatching2SetRoomDataInternal",0x80550006},
{"T6tnM1Uti4g","sceNpLookupNpId",0x80550006},
{"VqZX7POg2Mk","sceNpMatching2SearchRoom",0x80550006},
{"XDncXQIJUSk","sceNpGetOnlineId",0x80550006},
{"XVL8So3QJUk","connect",51},
{"YfmpW719rMo","sceNpMatching2CreateContext",0x80550006},
{"ZVw46bsasAk","sceNetEpollControl",0x80410133},
{"a8R9-75u4iM","sceNpGetAccountId",0x80550006},
{"aNeavPDNKzA","sendmsg",51},
{"bD-JizUb3JM","sceNpSignalingGetConnectionStatus",0x80550006},
{"bcoVwcBjQ9E","sceNpScoreRecordGameData",0x80550006},
{"drjIbDbA7UQ","sceNetEpollWait",0x80410133},
{"f42K37mm5RM","sceHttpsEnableOption",0},
{"gvD1greCu0A","sceNetSendto",0x80410133},
{"ilwLM4zOmu4","sceNpGetParentalControlInfo",0x80550006},
{"n5JmImxTiZU","sceNpMatching2JoinLobby",0x80550006},
{"oBr313PppNE","sendto",51},
{"or0e885BlXo","sceNpWebApiUtilityParseNpId",0x80550006},
{"p-o74CnoNzY","sceNpGetNpId",0x80550006},
{"q7GK98-nYSE","sceNpMatching2SetRoomDataExternal",0x80550006},
{"r4oAo9in0TA","sceNpScoreSanitizeComment",0x80550006},
{"r6MyYJkryz8","sceNpCheckPlus",0x80550006},
{"rJNPJqDCpiI","sceNpMatching2GetWorldInfoList",0x80550006},
{"rbknaUjpqWo","sceNpGetAccountIdA",0x80550006},
{"s2-NPIvz+iA","sceHttpSetNonblock",0},
{"tHD5FPFXtu4","sceNpMatching2SignalingGetConnectionStatus",0x80550006},
{"w21YgGGNtBk","sceNetEpollAbort",0x80410133},
{"wUmwXZHaX1w","sceNpMatching2SignalingGetPingInfo",0x80550006},
{"wyvlEgZ-55w","sceNpMatching2GetLobbyInfoList",0x80550006},
{"zCWZmXXN600","sceNpMatching2CreateJoinRoom",0x80550006},
{"zKoVok6FFEI","sceNpScoreGetGameData",0x80550006},
{"zT0XBtgtOSI","sceNpScoreRecordScore",0x80550006},
