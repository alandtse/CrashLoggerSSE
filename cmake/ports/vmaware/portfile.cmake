# header-only library
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO kernelwernel/VMAware
    REF 3659c330ad76f4d9705558d170a0697737f7ef3a
    SHA512 b4167ce304dd765117d0e898dc143fbb28dadee970468e04a5ff47f943fea57296af412146aabc46b479126ac4e862fb3078d692e2776a66562d8c9b7903432f
    HEAD_REF main
)

# Install codes
set(VMAWARE_SOURCE ${SOURCE_PATH}/src/vmaware.hpp)
file(INSTALL ${VMAWARE_SOURCE} DESTINATION ${CURRENT_PACKAGES_DIR}/include)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
