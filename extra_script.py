import time
import hashlib
import os
import shlex
import shutil
import platform as platformlib

from firmware_image import (
    esp_image_sha256,
    firmware_hash_kiss_frame,
)

#
# Helpier functions
#

def get_target():

    # Detect the operating system
    platform_system = platformlib.system().lower()
    #print("System:", platform_system)
    if "linux" in platform_system:
        os_name = "linux"
    elif "darwin" in platform_system:
        os_name = "darwin"
    else:
        os_name = "unknown"

    # Get OS release details
    try:
        platform_os_info = platformlib.freedesktop_os_release()
        #print("OS Release:", platform_os_info)
        if platform_os_info.get('VERSION_CODENAME'):
            distro_name = platform_os_info.get('ID') + "-" + platform_os_info.get('VERSION_CODENAME')
        else:
            distro_name = platform_os_info.get('ID')
        os_name += "-" + distro_name
    except Exception:
        pass

    # Detect the architecture
    platform_machine = platformlib.machine().lower()
    #print("Machine:", platform_machine)
    if platform_machine == "x86_64":
        arch_name = "amd64"
    elif "aarch" in platform_machine or "arm64" in platform_machine:
        arch_name = "arm64"
    elif "arm" in platform_machine:
        arch_name = "armhf"
    else:
        arch_name = "unknown"

    return os_name + "-" + arch_name


def find_rnodeconf():
    """Locate rnodeconf even when PlatformIO does not inherit its venv PATH."""
    discovered = shutil.which("rnodeconf")
    if discovered:
        return discovered

    # The project's documented local RNS installation uses this dedicated
    # virtualenv. PlatformIO runs from its own venv, so the executable is not
    # normally on PATH even though it is installed and usable.
    local_venv = os.path.expanduser(
        "~/.local/share/rnode-rns-venv/bin/rnodeconf"
    )
    if os.path.isfile(local_venv) and os.access(local_venv, os.X_OK):
        return local_venv
    return None


def run_rnodeconf(env, arguments):
    executable = find_rnodeconf()
    if executable is None:
        raise RuntimeError(
            "rnodeconf is required but was not found on PATH or in "
            "~/.local/share/rnode-rns-venv/bin"
        )
    command = " ".join(
        shlex.quote(str(part)) for part in [executable, *arguments]
    )
    result = env.Execute(command)
    if result != 0:
        # env.Execute only prints the child failure; without raising, SCons
        # reports the custom target as SUCCESS (observed during first Ozdisan
        # provisioning when the shell returned 127).
        raise RuntimeError(f"rnodeconf failed with exit status {result}")
    return result

#
# Custom targets
#

def target_package(target, source, env):
    print("*** Executing target_package steps...")
    print("Platform:", env.GetProjectOption("platform"))
    print("Board:", env.GetProjectOption("board"))
    print("Variant:", env.GetProjectOption("custom_variant"))
    #if env.GetProjectOption("custom_variant").endswith('_local'):
    #    print("*** Skipping target_package for local build")
    #    return
    # do some actions
    platform = env.GetProjectOption("platform")
    board = env.GetProjectOption("board")
    firmware_package(env)

#
# Upload actions
#

def pre_upload(source, target, env):
    print("*** Executing pre_upload steps...")
    # do some actions

def post_upload(source, target, env):
    print("*** Executing post_upload steps...")
    print("Platform:", env.GetProjectOption("platform"))
    print("Board:", env.GetProjectOption("board"))
    print("Variant:", env.GetProjectOption("custom_variant"))
    print("Serial port:", env.subst("$UPLOAD_PORT"))
    # do some actions
    platform = env.GetProjectOption("platform")
    board = env.GetProjectOption("board")
    if ("espressif32" in platform):
        # Get the firmware actually running first: provisioning and the hash
        # write below both speak KISS, which only the application answers.
        boot_usb_jtag_app(env)
        time.sleep(10)
        # device provisioning is incomplete and only currently appropriate for 915MHz T-Beam
        #device_wipe(env)
        device_provision(env)
        if upload_leaves_device_in_bootloader(env):
            firmware_hash_write_notice(None, env)
        else:
            firmware_hash(source, env)
        # firmware pacakaging is incomplete due to missing console image
        #firmware_package(env)
    elif ("nordicnrf52" in platform):
        time.sleep(10)
        # device provisioning is incomplete and only currently appropriate for 915MHz RAK4631
        #device_wipe(env)
        device_provision(env)
        time.sleep(5)
        firmware_hash(source, env)
        # firmware pacakaging is incomplete due to missing console image
        #firmware_package(env)

def pre_clean(env):
    print("*** Executing pre_clean steps...")
    print("Platform:", env.GetProjectOption("platform"))
    print("Board:", env.GetProjectOption("board"))
    print("Variant:", env.GetProjectOption("custom_variant"))
    project_dir = env.subst("$PROJECT_DIR")
    print("project_dir:", project_dir)
    env.Execute("rm -f " + project_dir + "/Release/" + env.subst("$PROGNAME") + ".zip")
    env.Execute("rm -f " + project_dir + "/Debug/" + env.subst("$PROGNAME") + ".elf")
    env.Execute("rm -f " + project_dir + "/Debug/" + env.subst("$PROGNAME") + ".map")
    env.Execute("rm -f " + project_dir + "/Release/" + env.subst("$PROGNAME") + "_debug.zip")

def full_clean(env):
    print("*** Executing full_clean steps...")
    project_dir = env.subst("$PROJECT_DIR")
    print("project_dir:", project_dir)
    env.Execute("rm -f " + project_dir + "/Release/release.json")

def device_wipe(env):
    # Device wipe
    print("--- Wiping Device ---")
    env.Execute("rnodeconf --eeprom-wipe " + env.subst("$UPLOAD_PORT"))

def boot_usb_jtag_app(env):
    """Start the application after an upload over native USB-Serial/JTAG.

    esptool finishes with "Hard resetting via RTS pin", but on some ESP32-S3
    boards that leaves the chip latched in the ROM downloader rather than
    running the freshly written image. Everything after the upload then talks
    to the ROM instead of the firmware: provisioning times out, the hash write
    goes nowhere, and the board looks dead while being perfectly healthy.
    Measured on the second Rev 2 (N16R2), where the app only started after a
    physical S1 press -- which is exactly the manual step native USB is
    supposed to remove.

    The sequence below -- DTR deasserted, pulse RTS -- is the one measured to
    start the application after an upload, verified across repeated flashes of
    the second Rev 2 (N16R2). The previous sequence asserted DTR instead, and
    esptool could still connect with --before no_reset afterwards, proving the
    downloader had never been left.

    Stated as a measurement, not as a rule about which line drives what: the
    behaviour depends on the open transition and on whether the ROM or the
    application owns the endpoint. Note that tools/ifac/provision.py, which
    talks to the *running* application, requires the opposite DTR state --
    see the note there. Same two lines, different owner, different meaning.
    """
    try:
        if env.GetProjectOption("custom_usb_jtag_reset") not in ("yes", "true", "1"):
            return
    except Exception:
        return
    port_path = env.subst("$UPLOAD_PORT")
    if not port_path:
        return
    print("*** Booting the application over USB-JTAG (%s)" % port_path)
    try:
        import serial
        port = serial.Serial(baudrate=115200, timeout=0.3,
                             dsrdtr=False, rtscts=False)
        port.port = port_path
        port.dtr = False         # IO0 high: boot the app, not the downloader
        port.rts = False
        port.open()
        port.dtr = False         # hold IO0 high across the whole pulse
        port.rts = True          # EN low: reset
        time.sleep(0.2)
        port.rts = False         # release EN
        port.close()
    except Exception as error:
        print("    could not drive the reset lines: %s" % error)
        print("    press the board's S1 (reset) button to start the firmware")
        return
    # The reset re-enumerates the USB device, so wait for it to come back
    # before anything downstream tries to open it.
    deadline = time.time() + 25.0
    while time.time() < deadline and not os.path.exists(port_path):
        time.sleep(0.5)
    time.sleep(3.0)


def upload_leaves_device_in_bootloader(env):
    """True when the upload flags stop the board rebooting into the app.

    The hash below is written over KISS, which only the running firmware
    answers. An env that uploads with --after=no_reset leaves the ROM
    bootloader on the port instead, so the write goes nowhere -- silently.
    """
    try:
        flags = env.GetProjectOption("upload_flags")
    except Exception:
        return False
    if not flags:
        return False
    if isinstance(flags, str):
        flags = flags.split()
    return any("--after" in f and "no_reset" in f for f in flags)

def firmware_hash_write_notice(hex_hash, env):
    port_path = env.subst("$UPLOAD_PORT")
    print("")
    print("*** FIRMWARE HASH NOT WRITTEN ***")
    print("This environment uploads with --after=no_reset, so the board is still")
    print("in the bootloader and cannot receive the hash over KISS.")
    print("")
    print("Until the hash is stored, device_init() fails its firmware check,")
    print("hw_ready stays 0 and the radio WILL NOT START. The node still joins")
    print("over WiFi and serves pages, so it looks healthy while being deaf on RF.")
    print("")
    print("Power-cycle the board out of download mode, then run:")
    print("")
    print("    pio run -e %s -t fixhash --upload-port %s"
          % (env.subst("$PIOENV"), port_path))
    print("")

def device_set_firmware_hash(firmware_hash, env):
    import serial

    port_path = env.subst("$UPLOAD_PORT")
    variant = env.GetProjectOption("custom_variant")
    frame = firmware_hash_kiss_frame(firmware_hash)
    print("Writing firmware hash directly over KISS...")
    with serial.Serial(port_path, 115200, timeout=0.1) as port:
        # Opening native USB can reset an ESP32-S3. Drain startup output and
        # wait until setup() has reached the serial command loop. The original
        # ESP32 Özdisan fixture takes materially longer while loading its
        # on-device RNS state; a four-second write is silently discarded and
        # the next boot then fails firmware validation with hw_ready=0.
        boot_wait = 20.0 if variant == "ozdisan_esp32_espnow" else 4.0
        ready_at = time.monotonic() + boot_wait
        while time.monotonic() < ready_at:
            port.read(4096)
        port.write(frame)
        port.flush()
        # A running image that needs a new target hash is currently invalid.
        # device_save_firmware_hash() commits it and calls hard_reset() itself.
        # Sending another reset a second later leaves that command buffered
        # during the new boot and produces a confusing second reboot once the
        # serial loop starts. A valid image receiving the same hash needs no
        # reboot, so the host must not reset in either case.
        time.sleep(1)

def target_fixhash(target, source, env):
    """Write the built firmware's hash to a board that is already running.

    The companion to the notice above: use it after power-cycling a board that
    was flashed over UART.
    """
    build_dir = env.subst("$BUILD_DIR")
    prog = env.subst("$PROGNAME")
    source_file = "%s/%s.bin" % (build_dir, prog)
    print("--- Writing Firmware Hash to a running device ---")
    print("source_file:", source_file)
    firmware_data = open(source_file, "rb").read()
    try:
        calc_hash = esp_image_sha256(firmware_data)
    except ValueError as error:
        print("Unable to calculate firmware hash: %s" % error)
        return
    print("firmware_hash:", calc_hash.hex())
    print("NOTE: this must be the same build that is actually on the board. If the")
    print("      sources changed since the flash, re-flash rather than running this,")
    print("      or the stored hash will not match the running image and hw_ready")
    print("      will stay 0 for a different reason.")
    device_set_firmware_hash(calc_hash, env)
    print("Hash written; an invalid running image resets itself after commit.")

def device_provision(env):
    # Device provision
    print("--- Provisioning Device ---")
    platform = env.GetProjectOption("platform")
    print("Platform:", platform)
    board = env.GetProjectOption("board")
    print("Board:", board)
    variant = env.GetProjectOption("custom_variant")
    print("Variant:", variant)
    match variant:
        case "tbeam" | "tbeam_local":
            env.Execute("rnodeconf --product e0 --model e9 --hwrev 1 --rom " + env.subst("$UPLOAD_PORT"))
        case "lora32v21" | "lora32v21_local":
            env.Execute("rnodeconf --product b1 --model b9 --hwrev 1 --rom " + env.subst("$UPLOAD_PORT"))
        case "heltec32v4pa" | "heltec32v4pa_local":
            env.Execute("rnodeconf --product c3 --model c8 --hwrev 1 --rom " + env.subst("$UPLOAD_PORT"))
        case "heltec_tracker_v2" | "heltec_tracker_v2_local":
            env.Execute("rnodeconf --product c4 --model cb --hwrev 1 --rom " + env.subst("$UPLOAD_PORT"))
        case "rak4631" | "rak4631_local":
            env.Execute("rnodeconf --product 10 --model 12 --hwrev 1 --rom " + env.subst("$UPLOAD_PORT"))
        case "rak3401" | "rak3401_local":
            env.Execute("rnodeconf --product 10 --model 14 --hwrev 1 --rom " + env.subst("$UPLOAD_PORT"))
        case "techo" | "techo_local":
            env.Execute("rnodeconf --product 15 --model 17 --hwrev 1 --rom " + env.subst("$UPLOAD_PORT"))
        case "heltec_t114" | "heltec_t114_local":
            env.Execute("rnodeconf --product c2 --model c7 --hwrev 1 --rom " + env.subst("$UPLOAD_PORT"))
        case "impr_rad01_rev1" | "impr_rad01_rev2" | "ozdisan_esp32_espnow":
            # Local boards are provisioned as PRODUCT_HMBRW / MODEL_FE. App uploads
            # preserve its signed EEPROM identity, so do not rewrite it here.
            #
            # Deliberately NOT auto-provisioning when the EEPROM looks blank:
            # deciding that requires querying the board over KISS, and that link
            # is least reliable exactly when it would matter -- after a UART
            # upload the board sits halted in the bootloader and does not
            # answer. A query that failed and read as "blank" would overwrite a
            # real signed identity, which is unrecoverable. Provisioning is a
            # once-per-board manufacturing step; run `-t provision` explicitly.
            print("Preserving existing local-board EEPROM provisioning")
            print("  (a NEW board must be provisioned once first:")
            print("     pio run -e <env> -t provision --upload-port <port>")
            print("   without it the board boots with hw_ready=0, even though")
            print("   this upload reports success)")
        case _:
            print(f"Unknown board variant {variant}, can not provision device!")

def firmware_hash(source, env):
    # Firmware hash
    print("--- Updating Firmware Hash ---")
    source_file = source[0].get_abspath()
    platform = env.GetProjectOption("platform")
    print("Platform:", platform)
    if (platform == "nordicnrf52"):
        build_dir = env.subst("$BUILD_DIR")
        env.Execute("cd " + build_dir + "; unzip -o " + source_file + " " + env.subst("$PROGNAME") + ".bin")
        #source_file.replace(".zip", ".bin")
        source_file = build_dir + "/" + env.subst("$PROGNAME") + ".bin";
        print("source_file:", source_file)
        firmware_data = open(source_file, "rb").read()
        calc_hash = hashlib.sha256(firmware_data).digest()
        hex_hash = calc_hash.hex()
        print("firmware_hash:", hex_hash)
        env.Execute("rnodeconf --firmware-hash " + hex_hash + " " + env.subst("$UPLOAD_PORT"))
    else:
        print("source_file:", source_file)
        firmware_data = open(source_file, "rb").read()
        if env.GetProjectOption("custom_variant") in (
            "heltec_tracker_v2",
            "heltec_tracker_v2_local",
            "impr_rad01_rev1",
            "impr_rad01_rev2",
            "ozdisan_esp32_espnow",
        ):
            try:
                calc_hash = esp_image_sha256(firmware_data)
            except ValueError as error:
                print(f"Unable to calculate firmware hash: {error}")
                return
            print("firmware_hash:", calc_hash.hex())
            device_set_firmware_hash(calc_hash, env)
        else:
            calc_hash = hashlib.sha256(firmware_data[0:-32]).digest()
            part_hash = firmware_data[-32:]
            hex_hash = calc_hash.hex()
            print("firmware_hash:", hex_hash)
            if calc_hash == part_hash:
                env.Execute("rnodeconf --firmware-hash " + hex_hash + " " + env.subst("$UPLOAD_PORT"))
            else:
                print("Calculated hash does not match!")

def firmware_package(env):
    # Firmware package
    print("--- Packaging Firmware ---")
    platform = env.GetProjectOption("platform")
    print("Platform:", platform)
    board = env.GetProjectOption("board")
    print("Board:", board)
    variant = env.GetProjectOption("custom_variant")
    print("Variant:", variant)
    core_dir = env.subst("$CORE_DIR")
    print("core_dir:", core_dir)
    packages_dir = env.subst("$PACKAGES_DIR")
    print("packages_dir:", packages_dir)
    workspace_dir = env.subst("$WORKSPACE_DIR")
    print("workspace_dir:", workspace_dir)
    project_dir = env.subst("$PROJECT_DIR")
    print("project_dir:", project_dir)
    #build_dir = env.subst("$BUILD_DIR").get_abspath()
    build_dir = env.subst("$BUILD_DIR")
    print("build_dir:", build_dir)
    env.Execute("mkdir -p " + project_dir + "/Release")
    env.Execute("mkdir -p " + project_dir + "/Debug")
    if (platform == "espressif32"):
        #env.Execute("cp " + packages_dir + "/framework-arduinoespressif32/tools/partitions/boot_app0.bin " + build_dir + "/rnode_firmware_" + variant + ".boot_app0")
        env.Execute("cp ~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin " + build_dir + "/rnode_firmware_" + variant + ".boot_app0")
        env.Execute("cp " + build_dir + "/bootloader.bin " + build_dir + "/" + env.subst("$PROGNAME") + ".bootloader")
        env.Execute("cp " + build_dir + "/partitions.bin " + build_dir + "/" + env.subst("$PROGNAME") + ".partitions")
        env.Execute("rm -f " + project_dir + "/Release/" + env.subst("$PROGNAME") + ".zip")
        zip_cmd = "zip --junk-paths "
        zip_cmd += project_dir + "/Release/rnode_firmware_" + variant + ".zip "
        zip_cmd += project_dir + "/Release/esptool/esptool.py "
        zip_cmd += project_dir + "/Release/console_image.bin "
        zip_cmd += build_dir + "/" + env.subst("$PROGNAME") + ".bin "
        zip_cmd += build_dir + "/" + env.subst("$PROGNAME") + ".boot_app0 "
        zip_cmd += build_dir + "/" + env.subst("$PROGNAME") + ".bootloader "
        zip_cmd += build_dir + "/" + env.subst("$PROGNAME") + ".partitions "
        env.Execute(zip_cmd)
        env.Execute("cp " + build_dir + "/" + env.subst("$PROGNAME") + ".elf " + project_dir + "/Debug/.")
        env.Execute("cp " + build_dir + "/" + env.subst("$PROGNAME") + ".map " + project_dir + "/Debug/.")
        zip_cmd = "zip --junk-paths "
        zip_cmd += project_dir + "/Release/rnode_firmware_" + variant + "_debug.zip "
        zip_cmd += build_dir + "/" + env.subst("$PROGNAME") + ".elf "
        zip_cmd += build_dir + "/" + env.subst("$PROGNAME") + ".map "
        env.Execute(zip_cmd)
    elif (platform == "nordicnrf52"):
        env.Execute("cp " + build_dir + "/" + env.subst("$PROGNAME") + ".zip " + project_dir + "/Release/.")
    else:
        env.Execute("cp " + build_dir + "/" + env.subst("$PROGNAME") + " " + build_dir + "/rnoded")
        env.Execute("rm -f " + project_dir + "/Release/rnoded-" + get_target() + ".zip")
        zip_cmd = "zip --junk-paths "
        zip_cmd += project_dir + "/Release/rnoded-" + get_target() + ".zip "
        zip_cmd += build_dir + "/rnoded "
        zip_cmd += project_dir + "/rnoded.example.conf "
        zip_cmd += project_dir + "/rnoded.example.service "
        env.Execute(zip_cmd)
        get_target()
    env.Execute("python3 " + project_dir + "/release_hashes.py > " + project_dir + "/Release/release.json")

#
# Main script
#

Import("env")

env.Replace(PROGNAME="rnode_firmware_%s" % env.GetProjectOption("custom_variant"))
print("PROGNAME:", env.subst("$PROGNAME"))

print("*** Running custom script...")
platform = env.GetProjectOption("platform")
print("Platform:", platform)
targets = env.GetProjectOption("targets", [])
print("Targets:", targets)

# Clean
if env.IsCleanTarget():
    pre_clean(env)
    if "cleanall" in targets or "fullclean" in targets:
        full_clean(env)

def target_provision(target, source, env):
    """Write a local-board device identity into a blank EEPROM. Run ONCE per board.

    Separate from `upload` on purpose. The identity is signed and locked
    (ADDR_INFO_LOCK), and rewriting it on every flash would destroy it -- so the
    upload path preserves it and never writes. That leaves virgin boards needing
    this one explicit step, which is also how provisioning should work at
    production scale: serial numbers assigned deliberately and recorded, not
    generated by whichever machine happened to flash the board.
    """
    variant = env.GetProjectOption("custom_variant")
    # PRODUCT_HMBRW (0xF0) / MODEL_FE (0xFE), see Boards.h. hwrev tracks the
    # hardware revision distinguishes the two RAD revisions and the radio-less
    # Ozdisan acceptance fixture.
    hwrev = {
        "impr_rad01_rev1": 1,
        "impr_rad01_rev2": 2,
        "ozdisan_esp32_espnow": 3,
    }.get(variant)
    if hwrev is None:
        print(f"No provisioning profile for variant {variant}")
        return
    port = env.subst("$UPLOAD_PORT")
    if not port:
        print("No upload port. Pass --upload-port <port>.")
        return
    print(f"--- Provisioning {variant} as PRODUCT_HMBRW/MODEL_FE hwrev {hwrev} ---")
    print("This writes a signed device identity and is normally done once.")
    run_rnodeconf(env, [
        "--product", "f0",
        "--model", "fe",
        "--hwrev", str(hwrev),
        "--rom", port,
    ])

# Add custom targets
if (platform == "espressif32"):
    env.AddCustomTarget(
        name="package",
        dependencies="$BUILD_DIR/${PROGNAME}.bin",
        actions=[
            target_package
        ],
        title="Package",
        description="Package esp32 firmware for delivery"
    )
elif (platform == "nordicnrf52"):
    # remove --specs=nano.specs to allow exceptions to work
    if '--specs=nano.specs' in env['LINKFLAGS']:
        env['LINKFLAGS'].remove('--specs=nano.specs')
    env.AddCustomTarget(
        name="package",
        dependencies="$BUILD_DIR/${PROGNAME}.zip",
        actions=[
            target_package
        ],
        title="Package",
        description="Package nrf52 firmware for delivery"
    )
else:
    env.AddCustomTarget(
        name="package",
        dependencies="$BUILD_DIR/${PROGNAME}",
        actions=[
            target_package
        ],
        title="Package",
        description="Package native daemon for delivery"
    )

if platform == "espressif32":
    env.AddCustomTarget(
        name="fixhash",
        dependencies="$BUILD_DIR/${PROGNAME}.bin",
        actions=[target_fixhash],
        title="Write Firmware Hash",
        description="Write the built firmware's hash to an already-running board"
    )

if env.GetProjectOption("custom_variant") in (
    "impr_rad01_rev1",
    "impr_rad01_rev2",
    "ozdisan_esp32_espnow",
):
    env.AddCustomTarget(
        name="provision",
        dependencies=None,
        actions=[target_provision],
        title="Provision local board",
        description="Write device identity to a blank EEPROM (once per board)"
    )

# Register actions
env.AddPreAction("upload", pre_upload)
env.AddPostAction("upload", post_upload)
