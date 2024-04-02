# header-only library
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO kernelwernel/VMAware
    REF 8bba272f540895844239e6e92a394a8b0ef08096
    SHA512 dc0c17d861a55388bc7cbb0916ff9746db27302f694792b0841f97861953c2fc1b53042a6a65367a9d861c2db38a41b04c78eea5af466940a3112cb4fabba99c
    HEAD_REF main
)

# Install codes
set(VMAWARE_SOURCE ${SOURCE_PATH}/src/vmaware.hpp)
file(INSTALL ${VMAWARE_SOURCE} DESTINATION ${CURRENT_PACKAGES_DIR}/include)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
