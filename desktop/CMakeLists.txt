

########### install files ###############

install( PROGRAMS org.kde.kmux.desktop  DESTINATION ${KDE_INSTALL_APPDIR} )
install( FILES org.kde.kmux.appdata.xml DESTINATION ${KDE_INSTALL_METAINFODIR} )
install(FILES org.kde.kmux.desktop DESTINATION ${KDE_INSTALL_DATADIR}/kglobalaccel)

install(FILES kmux.notifyrc DESTINATION ${KDE_INSTALL_KNOTIFYRCDIR})
install( PROGRAMS kmuxrun.desktop  DESTINATION ${KDE_INSTALL_DATADIR}/kio/servicemenus )
