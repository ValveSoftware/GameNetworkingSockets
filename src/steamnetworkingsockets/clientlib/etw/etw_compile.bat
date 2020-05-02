p4 open steamnetworkingsockets_etw_events.h open steamnetworkingsockets_etw_events.rc steamnetworkingsockets_etw_eventsTEMP.BIN
"C:\Program Files (x86)\Windows Kits\10\bin\x86\mc.exe" -um steamnetworkingsockets_etw_events.man
p4 revert -a steamnetworkingsockets_etw_events*
