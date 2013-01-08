# --------------------------------------------------------------------
# General purpose functions.
# --------------------------------------------------------------------

# The functions below can be used to set/get a variable with a variable
# name. This can be useful to have something similar to a hash map.
#
# Example:
#   set_var "hash_$key" "$value"
#   value=$(get_var "hash_$key")

set_var() {
	eval "var_$1=\"$2\""
}

get_var() {
	eval echo \${var_$1}
}

add_to_list() {
	local list item
	list=$1
	item=$2


	case "$list" in
	$item|$item\ *|*\ $item\ *|*\ $item)
		;;
	*)
		if [ -z "$list" ]; then
			list=$item
		else
			list="$list $item"
		fi
		;;
	esac

	echo "$list"
}

# Helper function to checkif a given command is available.
#
# Below this function, higher-level helpers which check for sets of
# commands. They are responsible for displaying a message if a tool is
# missing, so the user can understand what's going on.

tool_installed() {
	local tool message checked var
	tool=$1
	message=$2

	# If we already checked for this tool, return. This way, we
	# don't display the message again.
	var="tool_$(echo "$tool" | sed -r 's/[^a-zA-Z0-9_]+/_/g')"
	checked=$(get_var $var)
	if [ "$checked" = "found" ]; then
		return 0
	fi
	if [ "$checked" = "not found" ]; then
		return 1
	fi

	if ! which $tool >/dev/null 2>&1; then
		if [ "$message" ]; then
			echo "ERROR: $tool not found" 1>&2
			printf "%s\n\n" "$message" 1>&2
		fi

		set_var $var "not found"
		return 1
	fi

	set_var $var "found"
	return 0
}

image_info_tools_installed() {
	local missing_tool
	missing_tool=0

	echo "--> Check for images handling tools availability"

	if ! tool_installed exiv2 "
exiv2 is required to read Exif from images. Please install this package
and re-run this script."; then
		missing_tool=1
	fi

	return $missing_tool
}

image_export_tools_installed() {
	local missing_tool
	missing_tool=0

	echo "--> Check for images handling tools availability"

	if ! tool_installed darktable-cli "
darktable-cli (shipped with darktable 1.1 and later) is required to
export RAW images to jpeg and PFM files. Please install this package and
re-run this script."; then
		missing_tool=1
	fi

	if ! tool_installed convert "
ImageMagick is required to check input images correctness. Please
install this package and re-run this script."; then
		missing_tool=1
	fi

	return $missing_tool
}

tethering_tools_installed() {
	local missing_tool
	missing_tool=0

	echo "--> Check for tethering tools availability"

	if ! tool_installed gphoto2 "
gphoto2 is needed if you want this script to automatically take the
required pictures."; then
		missing_tool=1
	fi

	if ! tool_installed awk "
awk is needed to parse gphot2(1) output."; then
		missing_tool=1
	fi

	return $missing_tool
}

profiling_tools_installed() {
	local missing_tool
	missing_tool=0

	echo "--> Check for profiling tools availability"

	if ! tool_installed darktable-cli "
darktable-cli (shipped with darktable 1.1 and later) is required to
export RAW images to jpeg and PFM files. Please install this package and
re-run this script."; then
		missing_tool=1
	fi

	if ! tool_installed gnuplot "
gnuplot is required to generate the graphs used to estimate the quality
of the presets. Please install this command and re-run this script."; then
		missing_tool=1
	fi

	return $missing_tool
}

database_tools_installed() {
	local missing_tool
	missing_tool=0

	echo "--> Check for database tools availability"

	if ! tool_installed sqlite3 "
sqlite3 is required to prepare a database allowing you to test the
generated presets. Please install this command and re-run this script."; then
		missing_tool=1
	fi

	if ! tool_installed awk "
awk is needed to prepare presets for database insertion."; then
		missing_tool=1
	fi

	return $missing_tool
}

internal_tools_available() {
	local missing_tool
	missing_tool=0

	echo "--> Check for internal tools availability"

	if ! tool_installed make "
make is required to build darktable tools dedicated to noise profiling.
Please install this command and re-run this script."; then
		missing_tool=1
	fi

	if ! tool_installed cc "
A compilator (eg. gcc) is required to build darktable tools dedicated to
noise profiling. Please install this command and re-run this script."; then
		missing_tool=1
	fi

	if [ "$missing_tool" = "1" ]; then
		return 1
	fi

	echo "--> Build profiling tools"
	make -C "$scriptdir"
}

# --------------------------------------------------------------------
# Input image file handling.
# --------------------------------------------------------------------

get_image_iso() {
	local iso

	tool_installed exiv2

	iso=$(exiv2 -g Exif.Photo.ISOSpeedRatings -Pt "$1" 2>/dev/null || :)

	if [ -z "$iso" -o "$iso" = "65535" ]; then
		iso=$(exiv2 -g Exif.Photo.RecommendedExposureIndex -Pt "$1" 2>/dev/null || :)
	fi

	# Then try some brand specific values if still not found.

	if [ "$iso" = "" ]; then
		case "$(get_image_camera_maker "$1")" in
		NIKON*)
			iso=$(exiv2 -g Exif.NikonIi.ISO -Pt "$1" 2>/dev/null || :)
			;;
		esac
	fi

	echo $iso
}

get_image_camera_maker() {
	local maker

	tool_installed exiv2

	maker=$(exiv2 -g Exif.Image.Make -Pt "$1" 2>/dev/null || :)
	echo $maker
}

get_image_camera_model() {
	local model

	tool_installed exiv2

	model=$(exiv2 -g Exif.Image.Model -Pt "$1" 2>/dev/null || :)
	echo $model
}

sort_iso_list() {
	local iso_list

	iso_list="$@"
	echo $(for iso in $iso_list; do echo $iso; done | sort -n)
}

# CAUTION: This function uses the following global variables:
#     o  profiling_dir

auto_set_profiling_dir() {
	local flag camera subdir
	flag=$1

	echo
	echo "===> Check profiling directory"

	if [ "$profiling_dir" ]; then
		if [ -d "$profiling_dir" ]; then
			profiling_dir=${profiling_dir%/}
			return 0
		else
			cat <<EOF
ERROR: Profiling directory doesn't exist:
$profiling_dir
EOF
			return 1
		fi
	fi

	if ! camera_is_plugged; then
		cat <<EOF
ERROR: Please specify a directory to read or write profiling images
(using the "$flag" flag) or plug your camera and turn it on.
EOF
		return 1
	fi

	camera=$(get_camera_name)
	subdir=$(echo $camera | sed -r 's/[^a-zA-Z0-9_]+/-/g')
	profiling_dir="/var/tmp/darktable-noise-profiling/$subdir/profiling"
	test -d "$profiling_dir" || mkdir -p "$profiling_dir"
}

# CAUTION: This function uses the following global variables:
#     o  profiling_dir
#     o  images_$iso
#     o  images_for_iso_settings

list_input_images() {
	local iso image images

	echo
	echo "===> List profiling input images"
	for image in "$profiling_dir"/*; do
		if [ "$image" = "$profiling_dir/*" ]; then
			# Directory empty.
			break
		fi

		case "$image" in
		*.[jJ][pP][gG])
			# Skip jpeg files, if any. Other files don't
			# have Exif and will be skept automatically.
			continue
			;;
		esac

		iso=$(get_image_iso "$image")
		if [ -z "$iso" ]; then
			# Not an image.
			continue
		fi

		echo "--> Found ISO $iso image: $image"

		# Record filename for this ISO setting.
		images=$(get_var "images_$iso")
		if [ -z "$image" ]; then
			images=$image
		else
			images="$images $image"
		fi
		set_var "images_$iso" "$images"

		# Add ISO setting to a list.
		images_for_iso_settings=$(add_to_list "$images_for_iso_settings" $iso)
	done

	images_for_iso_settings=$(sort_iso_list "$images_for_iso_settings")
}

export_large_jpeg() {
	local input output xmp
	input=$1
	output=$2
	xmp="$input.xmp"

	tool_installed darktable-cli

	rm -f "$output" "$xmp"
	darktable-cli "$input" "$output" 1>/dev/null 2>&1
	rm -f "$xmp"
}

export_thumbnail() {
	local input output xmp
	input=$1
	output=$2

	tool_installed convert

	convert "$input" -resize 1024x1024 "$output"
}

check_exposition() {
	local orig input over under ret convert_flags
	orig=$1
	input=$2

	ret=0

	# See: http://www.imagemagick.org/discourse-server/viewtopic.php?f=1&t=19805

	convert_flags="-channel RGB -threshold 99% -separate -append"

	over=$(convert "$input" $convert_flags -format "%[mean]" info: | cut -f1 -d.)
	if [ "$over" -a "$over" -lt 80 ]; then
		# Image not over-exposed.
		echo "\"$orig\" not over-exposed ($over)"
		ret=1
	fi

	under=$(convert "$input" -negate $convert_flags -format "%[mean]" info: | cut -f1 -d.)
	if [ "$under" -a "$under" -lt 80 ]; then
		# Image not under-exposed.
		echo "\"$orig\" not under-exposed ($under)"
		ret=1
	fi

	return $ret
}

# --------------------------------------------------------------------
# Camera tethering.
# --------------------------------------------------------------------

camera_is_plugged() {
	tool_installed gphoto2 && gphoto2 -a >/dev/null 2>&1
}

get_camera_name() {
	local camera
	if camera_is_plugged; then
		camera=$(gphoto2 -a | head -n 1 | sed -r s'/^[^:]+: //')
		echo $camera
	fi
}

get_camera_raw_setting() {
	local raw_setting
	raw_setting=$(gphoto2 --get-config /main/imgsettings/imageformat | awk '
/^Choice: [0-9]+ RAW$/ {
	id = $0;
	sub(/^Choice: /, "", id);
	sub(/ RAW$/, "", id);
	print id;
	exit;
}
')

	echo $raw_setting
}

get_camera_iso_settings() {
	local iso_settings
	iso_settings=$(gphoto2 --get-config /main/imgsettings/iso | awk '
/^Choice: [0-9]+ [0-9]+$/ {
	iso = $0;
	sub(/^Choice: [0-9]+ /, "", iso);
	print iso;
}
')

	echo $iso_settings
}

# CAUTION: This function uses the following global variables:
#     o  profiling_dir
#     o  force_profiling_shots
#     o  pause_between_shots
#     o  iso_settings
#     o  images_$iso
#     o  images_for_iso_settings

auto_capture_images() {
	local do_profiling_shots iso image images			\
	 profiling_note_displayed profiling_note answer camera		\
	 shots_per_iso shots_seq files raw_id i not_first_round

	tool_installed exiv2

	do_profiling_shots=0
	if [ -z "$images_for_iso_settings" -o "$force_profiling_shots" = "1" ]; then
		do_profiling_shots=1
	fi
	if [ "$iso_settings" ]; then
		for iso in $iso_settings; do
			images=$(get_var "images_$iso")
			if [ -z "$images" ]; then
				do_profiling_shots=1
			fi
		done
	else
		iso_settings=$images_for_iso_settings
	fi

	if [ "$do_profiling_shots" = "0" ]; then
		cat <<EOF

The script will use existing input images for the profiling. No more
shot will be taken.
EOF
		return 0
	fi

	# Check for the camera presence, and if no camera is found, ask
	# the user to plug in his camera or point to an appropriate
	# directory.

	profiling_note_displayed=0
	profiling_note="Important note about the required images:

    o  The subject must contain both under-exposed AND over-exposed
       areas. A possible subject could be a sunny window (or an in-door
       light) on half of the picture and a dark/shadowed in-door object on
       the other half.

    o  Disable auto-focus and put everything out of focus."

	if ! camera_is_plugged; then
		profiling_note_displayed=1
		cat <<EOF

Noise profiling requires at least a RAW image per required or supported
ISO setting.

Either:

    o  Plug your camera to this computer and, when detected, hit Return.
       This script will query the camera for supported ISO settings and
       take the appropriate images.

    o  Type Ctrl+C, take at least one image per supported ISO setting and
       put them in a dedicated directory. Then, re-run this script and be
       sure to indicate this directory by using the "-d" flag.

$profiling_note
EOF
		read answer
	fi
	while ! camera_is_plugged; do
		cat <<EOF
ERROR: No camera found by gphoto2(1)!

Retry or check gphoto2 documentation.
EOF
		read answer
	done

	# If we reach this part, a camera is plugged in and the user
	# wants us to take the pictures for him.

	# If he didn't specify any ISO settings, query the camera.
	if [ -z "$iso_settings" ]; then
		iso_settings=$(get_camera_iso_settings)
	fi
	iso_settings=$(sort_iso_list $iso_settings)

	camera=$(get_camera_name)

	# We are now ready to take pictures for each supported/wanted
	# ISO settings.

	# TODO: For now, we take one image per ISO setting. When
	# benchmark is automated, this value can be raised to 3 for
	# instance, and the benchmark script will choose the best
	# preset.
	shots_per_iso=1
	case $(uname -s) in
	Linux) shots_seq=$(seq $shots_per_iso) ;;
	*BSD)  shots_seq=$(jot $shots_per_iso) ;;
	esac

	# gphoto2(1) writes images to the current directory, so cd to
	# the profiling directory.
	cd "$profiling_dir"
	for iso in $iso_settings; do
		if [ "$force_profiling_shots" = 1 ]; then
			# Remove existing shots for this ISO setting.
			echo "--> (remove ISO $iso existing shots)"
			files=$(get_var "images_$iso")
			for file in $files; do
				rm -v $file
			done
			set_var "images_$iso" ""
		fi

		images=$(get_var "images_$iso")
		if [ "$images" ]; then
			# We already have images for this ISO setting,
			# continue with the next one.
			continue
		fi

		echo
		echo "===> Taking $shots_per_iso profiling shot(s) for \"$camera - ISO $iso\""
		if [ "$profiling_note_displayed" = "0" ]; then
			profiling_note_displayed=1
			cat <<EOF

$profiling_note
EOF
			read answer
		fi

		if [ -z "$raw_id" ]; then
			raw_id=$(get_camera_raw_setting)
		fi

		# This script will do $shots_seq shots for each ISO setting.
		for i in $shots_seq; do
			if [ "$pause_between_shots" -a "$not_first_round" ]; then
				echo "(waiting $pause_between_shots seconds before shooting)"
				sleep "$pause_between_shots"
			fi
			not_first_round=1

			gphoto2						\
			 --set-config /main/imgsettings/imageformat=$raw_id\
			 --set-config /main/imgsettings/iso=$iso	\
			 --filename="$iso-$i.%C"			\
			 --capture-image-and-download

			image=$(ls -t "$profiling_dir/$iso-$i".* | head -n 1)

			images=$(get_var "images_$iso")
			if [ -z "$image" ]; then
				images=$image
			else
				images="$images $image"
			fi
			set_var "images_$iso" "$images"
		done
	done
	cd -
}

# --------------------------------------------------------------------
# Database handling.
# --------------------------------------------------------------------

add_profile() {
	local database label_prefix label iso a0 a1 a2 b0 b1 b2 	\
	 bin1 bina0 bina1 bina2 binb0 binb1 binb2 floatdump

	database=$1; shift
	label_prefix=$1; shift
	iso=$1; shift
	a0=$1; shift
	a1=$1; shift
	a2=$1; shift
	b0=$1; shift
	b1=$1; shift
	b2=$1; shift
	label="$@"

	tool_installed sqlite3

	floatdump="$scriptdir/floatdump"

	bin1=$(echo 1.0f | $floatdump)
	bina0=$(echo $a0 | $floatdump)
	bina1=$(echo $a1 | $floatdump)
	bina2=$(echo $a2 | $floatdump)
	binb0=$(echo $b0 | $floatdump)
	binb1=$(echo $b1 | $floatdump)
	binb2=$(echo $b2 | $floatdump)

	echo "--> Adding \"$label\" to database"

	echo "insert into presets ("					\
	 "name,"							\
	 "description,"							\
	 "operation,"							\
	 "op_version,"							\
	 "op_params,"							\
	 "enabled,"							\
	 "blendop_params,"						\
	 "model,"							\
	 "maker,"							\
	 "lens,"							\
	 "iso_min,"							\
	 "iso_max,"							\
	 "exposure_min,"						\
	 "exposure_max,"						\
	 "aperture_min,"						\
	 "aperture_max,"						\
	 "focal_length_min,"						\
	 "focal_length_max,"						\
	 "writeprotect,"						\
	 "autoapply,"							\
	 "filter,"							\
	 "def,"								\
	 "isldr,"							\
	 "blendop_version"						\
	 ") "								\
	 "values ("							\
	 "'$label', '', 'denoiseprofile', 1, "				\
	 "X'${bin1}${bin1}${bina0}${bina1}${bina2}${binb0}${binb1}${binb2}', "\
	 "1, X'00', '', '', '', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4);" | \
	sqlite3 $database
}
