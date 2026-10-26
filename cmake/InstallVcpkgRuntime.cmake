# Install only release vcpkg runtime DLLs alongside the Windows executables.
option( KEMI_INSTALL_VCPKG_RUNTIME "Install release vcpkg runtime DLLs" ON )

if( WIN32 AND MSVC AND KEMI_INSTALL_VCPKG_RUNTIME
        AND VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET )
    # vcpkg resolves dependencies before this project is configured.  Reconfigure
    # if the release DLL set changes; never scan the triplet's debug/bin directory.
    file( GLOB _kemi_vcpkg_runtime_dlls CONFIGURE_DEPENDS LIST_DIRECTORIES FALSE
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/*.dll" )

    if( _kemi_vcpkg_runtime_dlls )
        install( FILES ${_kemi_vcpkg_runtime_dlls}
            DESTINATION "${KICAD_BIN}"
            COMPONENT binary )
    endif()

    unset( _kemi_vcpkg_runtime_dlls )
endif()
