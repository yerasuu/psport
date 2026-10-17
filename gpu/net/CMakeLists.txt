# psport online module: libpsport-net.so, the PS4 network and PSN libraries with the shadNet client
# (shadp2p, a shadPS4 fork, GPL-2.0), loaded by the runtime only for online play
# (src/net/runtime_net.c). Its headers (include/) extend the port's shim for these libraries alone;
# everything in it is hidden and bound to itself (-Bsymbolic) except psport_net.h's two calls, so its
# copies of shadPS4's kernel shims and symbol table never meet libpsport-gpu's.
# Needs protobuf, nlohmann_json, OpenSSL, zlib and miniupnpc (shell.nix); without them the port
# builds without it and plays offline (gpu/CMakeLists.txt: PS_NET).

set(SHADNET_PROTO_OUT "${CMAKE_CURRENT_BINARY_DIR}/shadnet_proto_gen")
file(MAKE_DIRECTORY "${SHADNET_PROTO_OUT}")
add_custom_command(
    OUTPUT "${SHADNET_PROTO_OUT}/shadnet.pb.cc" "${SHADNET_PROTO_OUT}/shadnet.pb.h"
    COMMAND protobuf::protoc "--proto_path=${CMAKE_CURRENT_SOURCE_DIR}/shadps4/shadnet"
            "--cpp_out=${SHADNET_PROTO_OUT}" "${CMAKE_CURRENT_SOURCE_DIR}/shadps4/shadnet/shadnet.proto"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shadps4/shadnet/shadnet.proto")
file(GLOB_RECURSE PSPORT_NET_SOURCES CONFIGURE_DEPENDS shadps4/core/libraries/network/*.cpp
    shadps4/core/libraries/np/*.cpp shadps4/shadnet/*.cpp)
# Trophies and the system dialogs stay in the C runtime; their shadPS4 versions need the
# emulator's UI layers.
list(FILTER PSPORT_NET_SOURCES EXCLUDE REGEX
    "np_trophy|trophy_ui|np_sns_facebook_dialog|np_profile_dialog/|np_commerce/")

# A title's own part of the module (Game::OnlineExtension): games/<game>/net/, its sources and its
# headers (each folder an include directory), started only for that title (psport_net_title.h).
file(GLOB_RECURSE PSPORT_NET_TITLE_SOURCES CONFIGURE_DEPENDS ../games/*/net/*.cpp)
file(GLOB PSPORT_NET_TITLE_DIRS LIST_DIRECTORIES true CONFIGURE_DEPENDS ../games/*/net)

add_library(psport-net SHARED ${PSPORT_NET_SOURCES} ${PSPORT_NET_TITLE_SOURCES} "${SHADNET_PROTO_OUT}/shadnet.pb.cc"
    psport_net_glue.cpp psport_net_title.cpp)
# The module's own headers first, then the port's shim and shadPS4 (common/, logging).
target_include_directories(psport-net PRIVATE include shadps4 shadps4/shadnet "${SHADNET_PROTO_OUT}"
    ${PSPORT_NET_TITLE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR} ../shim ../shadps4 ../third_party/imgui ../third_party/httplib)
# The version the shadNet client reports to the server (ShadNetClient::BuildVersionString). psport has
# no release numbers yet: its unstable version and commit, as packaging/port.nix names it; set on that
# file alone so that each commit rebuilds one source, not the module.
execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE PSPORT_NET_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
set(PSPORT_NET_VERSION "0-unstable")
if(PSPORT_NET_REVISION)
    string(APPEND PSPORT_NET_VERSION "-${PSPORT_NET_REVISION}")
endif()
set_property(SOURCE shadps4/shadnet/client.cpp APPEND PROPERTY COMPILE_DEFINITIONS
    PSPORT_NET_VERSION="${PSPORT_NET_VERSION}")
target_compile_definitions(psport-net PRIVATE BOOST_ASIO_STANDALONE NDEBUG
    CPPHTTPLIB_OPENSSL_SUPPORT CPPHTTPLIB_ZLIB_SUPPORT)
target_compile_options(psport-net PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden
    -Wno-deprecated-declarations -Wno-missing-field-initializers)
target_link_options(psport-net PRIVATE -Wl,-Bsymbolic -Wl,--exclude-libs,ALL)
# libpsport-gpu (already in the process) provides shadPS4's common code: logging, threads' names.
set_target_properties(psport-net PROPERTIES BUILD_RPATH "\$ORIGIN" INSTALL_RPATH "\$ORIGIN"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}") # beside libpsport-gpu.so, where the runtime looks
target_link_libraries(psport-net PRIVATE psport-gpu nlohmann_json::nlohmann_json protobuf::libprotobuf
    OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB "${MINIUPNPC_LIBRARY}" Vulkan::Headers fmt::fmt
    Boost::headers magic_enum::magic_enum)
target_include_directories(psport-net PRIVATE "${MINIUPNPC_INCLUDE}")
