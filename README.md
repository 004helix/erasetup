**erasetup** is utility used to setup volumes and manage snapshots for dm-era

**Usage:**

	erasetup [-h|--help] [-v|--verbose] [-f|--force]
	         <command> [command options]
	
	         create <name> <metadata-dev> <data-dev> [chunk-size]
	         open <name> <metadata-dev> <data-dev>
	         close <name>
	         status [name]
	         dumpmeta <metadata-dev>
	
	         takesnap <name> <snapshot-dev>
	         dropsnap <snapshot-dev>
	         dumpsnap <metadata-dev>

**Create device example:**

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

**Take snapshot example:**

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

**Drop snapshot example**

	# erasetup dropsnap /dev/vg/snap
	# erasetup status
	name:          home
	current era:   3
	device size:   4.00 GiB
	chunk size:    64.00 KiB
	metadata size: 32.00 MiB
	metadata used: 300.00 KiB (0.92%)
	uuid:          ERA-253-2

**Dump snapshot example**

	# erasetup dumpsnap /dev/vg/snap
	<snapshot block_size="128" blocks="65536" era="12"
	          dev="/dev/mapper/era-snap-45c62c3e66-8b2d-880d-ef53-48138498b3">
	  <block block="0" era="12"/>
	  <range begin="1" end="31" era="1"/>
	  <range begin="32" end="34" era="3"/>
	  <range begin="35" end="545" era="1"/>
	  <block block="546" era="3"/>
	  <range begin="547" end="2047" era="0"/>
	  <block block="2048" era="1"/>
	  <range begin="2049" end="2111" era="0"/>
	  <range begin="2112" end="3711" era="3"/>
	  <range begin="3712" end="6143" era="0"/>
	  <block block="6144" era="1"/>
	  <range begin="6145" end="10239" era="0"/>
	  <block block="10240" era="1"/>
	  <range begin="10241" end="14335" era="0"/>
	  <block block="14336" era="1"/>
	  <range begin="14337" end="18431" era="0"/>
	  <block block="18432" era="1"/>
	  <range begin="18433" end="30719" era="0"/>
	  <block block="30720" era="4"/>
	  <block block="30721" era="3"/>
	  <range begin="30722" end="32768" era="1"/>
	  <block block="32769" era="0"/>
	  <range begin="32770" end="33281" era="1"/>
	  <range begin="33282" end="51199" era="0"/>
	  <block block="51200" era="1"/>
	  <range begin="51201" end="55295" era="0"/>
	  <block block="55296" era="1"/>
	  <range begin="55297" end="65534" era="0"/>
	  <block block="65535" era="1"/>
	</snapshot>

**Close device example**

	# erasetup close home

**Open device example**

	# erasetup open home /dev/vg/meta /dev/vg/data
