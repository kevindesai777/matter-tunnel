#
# Example application: Nordic's Matter Light Bulb, with the VendorTunnel cluster
# dropped in.
#
# The second device type. Nothing in lib/tunnel/ differs between this and
# examples/lock -- that is the point of the exercise. What differs here is only
# what any bulb needs: three upstream sources instead of the lock's four, PWM for
# the brightness LED, and a Full Thread Device rather than a Minimal one.
#
# This is deliberately a *thin* example. It compiles the upstream application's
# C++ sources in place rather than copying them, so that:
#   - the upstream application is never modified, preserving its provenance;
#   - the only new source in the build is the tunnel handler itself, which is
#     the honest measure of what dropping this component in actually costs.
#
# SPDX-License-Identifier: Apache-2.0
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(matter-light-bulb-tunnel)

# Path to the unmodified upstream light bulb sample. Override with
# -DUPSTREAM_APP_DIR=... if the workspace lives elsewhere.
if(NOT DEFINED UPSTREAM_APP_DIR)
  set(UPSTREAM_APP_DIR
      "$ENV{HOME}/ncs-door-lock/project-workspace/nrf/samples/matter/light_bulb")
endif()

if(NOT EXISTS "${UPSTREAM_APP_DIR}/src/app_task.cpp")
  message(FATAL_ERROR
    "Upstream light bulb sample not found at ${UPSTREAM_APP_DIR}. "
    "Pass -DUPSTREAM_APP_DIR=<path> pointing at nrf/samples/matter/light_bulb.")
endif()

# Root of this component (…/matter-tunnel)
get_filename_component(TUNNEL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# Enable GNU STD support.
include(${ZEPHYR_CONNECTEDHOMEIP_MODULE_DIR}/config/nrfconnect/app/enable-gnu-std.cmake)

# NORDIC SDK APP START

include(${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common/cmake/source_common.cmake)
include(${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common/cmake/data_model.cmake)

include(${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common/cmake/zap_helpers.cmake)
ncs_get_zap_parent_dir(ZAP_PARENT_DIR)
zephyr_include_directories(${ZAP_PARENT_DIR})

# A manufacturer-specific cluster needs its own C++ types (Ids.h, cluster
# objects, callbacks). Those live in app-common, and the SDK's pre-generated
# zzz_generated/app-common only knows standard clusters. So the data model must
# be generated with `west zap-generate -f` (full), and CHIP_APP_ZAP_DIR
# redirected here, or the build fails with
#   'chip::app::Clusters::VendorTunnel' has not been declared
# from the generated callback-stub.cpp. Matches the approach in the nRF Connect
# SDK manufacturer_specific sample.
get_filename_component(CHIP_APP_ZAP_DIR ${ZAP_PARENT_DIR}/zap-generated REALPATH CACHE)

target_include_directories(app PRIVATE
  ${UPSTREAM_APP_DIR}/src
  ${ZAP_PARENT_DIR}
  # Must precede the SDK's zzz_generated/app-common on the include path.
  # Both provide <app-common/zap-generated/...>, but only this one contains the
  # manufacturer-specific cluster's C++ types; resolving to the SDK copy yields
  # "'VendorTunnel' does not name a type" in the handler.
  ${ZAP_PARENT_DIR}/zap-generated
)

target_sources(app PRIVATE
  # upstream sample, unmodified. Three sources, not the lock's four: there is no
  # equivalent of bolt_lock_manager.cpp. The fourth entry is an SDK source the
  # bulb sample compiles directly, for deferred attribute persistence.
  ${UPSTREAM_APP_DIR}/src/app_task.cpp
  ${UPSTREAM_APP_DIR}/src/main.cpp
  ${UPSTREAM_APP_DIR}/src/zcl_callbacks.cpp
  ${CHIP_ROOT}/src/app/persistence/DeferredAttributePersistenceProvider.cpp

  # the drop-in component — the only additions.
  # The cluster and its opaque envelope; always present.
  ${TUNNEL_ROOT}/lib/tunnel/tunnel_server.cpp
  ${TUNNEL_ROOT}/lib/tunnel/tunnel_cluster_objects.cpp
)

# The security layer. Excluded wholesale when CONFIG_TUNNEL_SECURITY=n, which is
# how the transport-only footprint stage is built. tunnel_bench.cpp belongs here
# rather than under CONFIG_TUNNEL_CRYPTO_BENCHMARK: it includes tunnel_crypto.h
# unconditionally, and the linker discards its body when the benchmark is off.
if(CONFIG_TUNNEL_SECURITY)
  target_sources(app PRIVATE
    ${TUNNEL_ROOT}/lib/tunnel/tunnel_crypto.cpp
    ${TUNNEL_ROOT}/lib/tunnel/tunnel_bench.cpp
    ${TUNNEL_ROOT}/lib/tunnel/tunnel_identity.cpp
    ${TUNNEL_ROOT}/lib/tunnel/tunnel_envelope.cpp
    ${TUNNEL_ROOT}/lib/tunnel/tunnel_replay.cpp
  )
endif()

# Client key registration and inbound request verification.
if(CONFIG_TUNNEL_CLIENT_AUTH)
  target_sources(app PRIVATE ${TUNNEL_ROOT}/lib/tunnel/tunnel_client_key.cpp)
endif()

# Second transport adapter, for the BLE floor measurement (condition 0).
# Compiled only when the NUS service is configured in, so the Matter-only build
# is byte-for-byte unaffected.
if(CONFIG_BT_NUS)
  target_sources(app PRIVATE ${TUNNEL_ROOT}/lib/tunnel/tunnel_ble.cpp)
endif()

ncs_configure_data_model(
  EXTERNAL_CLUSTERS "VENDOR_TUNNEL_CLUSTER"
)

# NORDIC SDK APP END
