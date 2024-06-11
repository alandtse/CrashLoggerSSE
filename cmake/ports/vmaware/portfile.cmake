# header-only library
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO kernelwernel/VMAware
    REF 7f76484bf8fd4d287bba176fe6eb42daeb374127
    SHA512 326792da52e550becfa9e36c0d804b7c22806de5fc4bc6ab4345f7c15e4f60cc9261b38e31693d364fc33534b233cb9b96002c18a60f7a80a6a0d14c023fd774
    HEAD_REF main
)

# Install codes
set(VMAWARE_SOURCE ${SOURCE_PATH}/src/vmaware.hpp)
file(INSTALL ${VMAWARE_SOURCE} DESTINATION ${CURRENT_PACKAGES_DIR}/include)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
