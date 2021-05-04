# ---------- trusty ----------
# Above line is mandatory!
# rules to build deb package for Ubuntu 14.04 (Trusty)
# This file is retired and will be deleted soon.

message ( STATUS "Will create DEB for Ubuntu 14.04 (Trusty)" )

# m.b. postinst.xenial, postinst.debian and postinst.trusty
FILE ( READ dist/deb/postinst.trusty.in POSTINST_SPECIFIC)

# m.b. prerm.ubuntu, prerm.debian
configure_file ( "${CMAKE_CURRENT_SOURCE_DIR}/dist/deb/prerm.ubuntu.in"
		"${MANTICORE_BINARY_DIR}/prerm" @ONLY )

# m.b. posrtrm.systemd, posrtm.wheezy targeted to posrtm, and also preinst.trusty targeted to preinst
configure_file ( "${CMAKE_CURRENT_SOURCE_DIR}/dist/deb/preinst.trusty.in"
		"${MANTICORE_BINARY_DIR}/preinst" @ONLY )

# upstart is only for trusty, newer switched to systemd
configure_file ( "${CMAKE_CURRENT_SOURCE_DIR}/dist/deb/manticore.upstart.in"
		"${MANTICORE_BINARY_DIR}/manticore.upstart" @ONLY )

# m.b. posrtrm, preinst - see also above
set ( EXTRA_SCRIPTS "${MANTICORE_BINARY_DIR}/preinst;" )

include ( builds/CommonDeb )

configure_file ( "${CMAKE_CURRENT_SOURCE_DIR}/dist/deb/manticore.service.in"
               "${MANTICORE_BINARY_DIR}/manticore.service" @ONLY )

install ( FILES "${MANTICORE_BINARY_DIR}/manticore.service"
               DESTINATION /lib/systemd/system COMPONENT applications )

configure_file ( "${CMAKE_CURRENT_SOURCE_DIR}/dist/deb/manticore.generator.in"
		"${MANTICORE_BINARY_DIR}/manticore-generator" @ONLY )

install ( FILES "${MANTICORE_BINARY_DIR}/manticore.upstart"
                DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/init COMPONENT applications RENAME manticore.conf )

install ( FILES "${MANTICORE_BINARY_DIR}/manticore-generator"
		DESTINATION  /lib/systemd/system-generators  PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
        GROUP_EXECUTE GROUP_READ COMPONENT applications )

# some trusty-specific variables and files
set ( DISTR_SUFFIX "~trusty_${CPACK_DEBIAN_PACKAGE_ARCHITECTURE}" )

set ( CPACK_DEBIAN_PACKAGE_SUGGESTS "libmysqlclient18, libpq5, libexpat1, libodbc1" )
set ( CPACK_DEBIAN_TOOLS_PACKAGE_SUGGESTS "libmysqlclient18, libpq5, libexpat1, libodbc1" )
set ( CPACK_DEBIAN_TOOLS_PACKAGE_RECOMMENDS "manticore-icudata" )
set ( CPACK_DEBIAN_BIN_PACKAGE_RECOMMENDS "manticore-icudata" )