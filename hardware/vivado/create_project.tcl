# Recreate the current Zybo Z7-20 Vivado hardware project from repository sources.

set script_dir   [file normalize [file dirname [info script]]]
set hardware_dir [file normalize "$script_dir/.."]
set repo_root    [file normalize "$hardware_dir/.."]

set project_name "zybo_z7_20_dma_xor"
set project_dir  [file normalize "$repo_root/build/vivado/$project_name"]

set rtl_file     [file normalize "$hardware_dir/rtl/zybo_axis_xor_accel.v"]
set ip_repo_dir  [file normalize "$hardware_dir/ip/zybo_accel_ctrl_1_0"]
set bd_tcl_file  [file normalize "$script_dir/create_block_design.tcl"]

foreach required_path [list $rtl_file $ip_repo_dir $bd_tcl_file] {
    if {![file exists $required_path]} {
        error "Required repository input not found: $required_path"
    }
}

if {[file exists $project_dir]} {
    error "Vivado project directory already exists: $project_dir"
}

file mkdir $project_dir

create_project $project_name $project_dir -part xc7z020clg400-1
set_property board_part digilentinc.com:zybo-z7-20:part0:1.2 [current_project]

# Load the packaged custom AXI-Lite control IP before recreating the block design.
set_property IP_REPO_PATHS [list $ip_repo_dir] [current_fileset]
update_ip_catalog -rebuild

# Add the hand-written AXI-Stream validation accelerator used by the block design.
add_files -norecurse $rtl_file

# Recreate the IP Integrator block design from the exported Tcl description.
source $bd_tcl_file

set bd_designs [get_bd_designs -quiet system]
if {[llength $bd_designs] != 1} {
    error "Expected recreated block design 'system', found [llength $bd_designs]."
}

set bd_design [lindex $bd_designs 0]
set bd_file [get_files -quiet [get_property FILE_NAME $bd_design]]
if {[llength $bd_file] != 1} {
    error "Expected one block-design file for 'system', found [llength $bd_file]."
}

set bd_name [get_property NAME $bd_design]

# Generate block-design outputs and create the top-level HDL wrapper.
generate_target all $bd_file
make_wrapper -files $bd_file -top -import

set_property top "${bd_name}_wrapper" [current_fileset]
update_compile_order -fileset sources_1

puts "Vivado project created successfully:"
puts "  $project_dir"
