file(GLOB KMUX_ICONS "${CMAKE_CURRENT_LIST_DIR}/*-apps-kmux.png" "${CMAKE_CURRENT_LIST_DIR}/sc-apps-kmux.svg")

include(ECMInstallIcons)
ecm_install_icons(ICONS ${KMUX_ICONS} DESTINATION ${KDE_INSTALL_ICONDIR})
