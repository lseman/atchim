# Bootstrap a checksum-pinned CPM release into the build directory.
set(ATCHIM_CPM_VERSION 0.43.2)
set(ATCHIM_CPM_FILE "${CMAKE_BINARY_DIR}/cmake/CPM_${ATCHIM_CPM_VERSION}.cmake")
if(NOT EXISTS "${ATCHIM_CPM_FILE}")
    file(DOWNLOAD
        "https://github.com/cpm-cmake/CPM.cmake/releases/download/v${ATCHIM_CPM_VERSION}/CPM.cmake"
        "${ATCHIM_CPM_FILE}"
        EXPECTED_HASH SHA256=49a3bef91ceb65bb66d57255e12d1ffd22abc2f6408fa9fe4534c544a2f232aa
        TLS_VERIFY ON)
endif()
include("${ATCHIM_CPM_FILE}")
