# header-only library
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO kernelwernel/VMAware
    REF 2864b64ccc4180768c874dae312ab61bc16bd221
    SHA512 351c3bbea09c0635900731e74b2aabc7e4548550f36fee81d752c5fea4765042b13ae76015818bd3c02ab837292ffb3a1f46b459bec814664ecc15afbbfa70fa
    HEAD_REF main
)

# Install codes
set(VMAWARE_SOURCE ${SOURCE_PATH}/src/vmaware.hpp)
file(INSTALL ${VMAWARE_SOURCE} DESTINATION ${CURRENT_PACKAGES_DIR}/include)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
