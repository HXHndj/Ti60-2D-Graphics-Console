reset_timing
read_sdc cdc_physical.sdc
report_bus_skew -npaths 1024 -nworst 1 -file scan_bus_skew.rpt
