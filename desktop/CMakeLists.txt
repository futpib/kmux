

########### install files ###############

include(${CMAKE_CURRENT_SOURCE_DIR}/kmux-metadata.cmake)

install( PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/org.kde.kmux.desktop DESTINATION ${KDE_INSTALL_APPDIR} )
install( FILES ${CMAKE_CURRENT_BINARY_DIR}/org.kde.kmux.appdata.xml DESTINATION ${KDE_INSTALL_METAINFODIR} )
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.kde.kmux.desktop DESTINATION ${KDE_INSTALL_DATADIR}/kglobalaccel)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/kmux.notifyrc DESTINATION ${KDE_INSTALL_KNOTIFYRCDIR})
install( PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/kmuxrun.desktop DESTINATION ${KDE_INSTALL_DATADIR}/kio/servicemenus )

include(${CMAKE_SOURCE_DIR}/data/icons/kmux-icons.cmake)
