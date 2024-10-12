# header-only library
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO kernelwernel/VMAware
    REF 8a077fbffb142bb5d2b98c2c307f583352a5748a
    SHA512 0eac443240637a89af67034d9d806c9b92ac6bc91ae4d9f216c20ac84b07c1f8968397b42c0618265f6a723742bad8a6f96b921c89b74c11781b6fedeb43b6f9
    HEAD_REF main
)

# Install codes
set(VMAWARE_SOURCE ${SOURCE_PATH}/src/vmaware.hpp)
file(INSTALL ${VMAWARE_SOURCE} DESTINATION ${CURRENT_PACKAGES_DIR}/include)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
