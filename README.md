**erasetup** is utility used to setup volumes and manage snapshots for dm-era

# Usage:

	erasetup [-h|--help] [-v|--verbose] [-f|--force]
	         <command> [command options]
	
	         create <name> <metadata-dev> <data-dev> [chunk-size]
	         open <name> <metadata-dev> <data-dev>
	         close <name>
	         status [name]
	
	         takesnap <name> <snapshot-dev>
	         dropsnap <snapshot-dev>
	
	         dumpsb <metadata-dev>

# Create device example:

	# lvcreate -L 32M -n meta vg
	  Logical volume "meta" created.
	# lvcreate -L 4G -n data vg
	  Logical volume "data" created.
	# erasetup create home /dev/vg/meta /dev/vg/data
	# erasetup status
	name:          home
	current era:   1
	device size:   4.00 GiB
	chunk size:    64.00 KiB
	metadata size: 32.00 MiB
	metadata used: 300.00 KiB (0.92%)
	uuid:          ERA-253-2
	
	# mkfs.ext4 -q /dev/mapper/home
	# mount /dev/mapper/home /home

# Take snapshot example:

	# lvcreate -L 1G -n snap vg
	  Logical volume "snap" created.
	# erasetup takesnap home /dev/vg/snap
	# dd if=/dev/zero of=/home/test bs=1M count=100
	100+0 records in
	100+0 records out
	104857600 bytes (105 MB) copied, 0.0889242 s, 1.2 GB/s
	# erasetup status
	name:          home
	current era:   3
	device size:   4.00 GiB
	chunk size:    64.00 KiB
	metadata size: 32.00 MiB
	metadata used: 300.00 KiB (0.92%)
	uuid:          ERA-253-2
	
	  snapshot:    45c62c3e66-8b2d-880d-ef53-48138498b3
	  status:      Active
	  size:        1023.74 MiB
	  used:        100.11 MiB (9.78%)
	  era:         2

# Drop snapshot example

	# erasetup dropsnap /dev/vg/snap
	# erasetup status
	name:          home
	current era:   3
	device size:   4.00 GiB
	chunk size:    64.00 KiB
	metadata size: 32.00 MiB
	metadata used: 300.00 KiB (0.92%)
	uuid:          ERA-253-2

# Close device example

	# erasetup close home

# Open device example

	# erasetup open home /dev/vg/meta /dev/vg/data
