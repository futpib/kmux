file(GLOB KMUX_ICONS "*-apps-kmux.png" "sc-apps-kmux.svg")

include(ECMInstallIcons)
ecm_install_icons(ICONS ${KMUX_ICONS} DESTINATION ${KDE_INSTALL_ICONDIR})
