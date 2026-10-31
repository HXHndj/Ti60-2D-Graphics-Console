reset_timing
read_sdc text_cdc.sdc
report_timing -setup -from [get_pins {text_layer/mailbox/hold_data[*]~FF|CLK}] -to [get_pins {text_layer/received[*]~FF|D}] -npaths 1024 -nworst 1 -file text_metadata.rpt
