import struct, json, os, subprocess

GOD_MAGIC = 0x534C524F4F544F44


def build_god_anchor(layout):
    raw_anchor = struct.pack("<QQQII", GOD_MAGIC, 1, 2, 0, 0)
    return raw_anchor + b"\x00" * (layout["sector_size_bytes"] - len(raw_anchor))


def create_deployable_media():
    with open("deploy.json", "r") as f:
        config = json.load(f)
    layout = config["disk_layout"]
    subprocess.run(["make", "clean"], check=True)
    subprocess.run(["make", "my_sls_kernel.bin"], check=True)
    target_disk = "sls_dist_release.img"
    with open(target_disk, "wb") as disk:
        disk.truncate(4 * 1024 * 1024 * 1024)
    with open(target_disk, "r+b") as disk:
        for payload in config["payloads"]:
            with open(payload["source_file"], "rb") as pf:
                binary_data = pf.read()
            disk.seek(payload["target_sector_offset"] * layout["sector_size_bytes"])
            disk.write(binary_data)
        anchor_payload = build_god_anchor(layout)
        disk.seek(layout["god_anchor_sector"] * layout["sector_size_bytes"])
        disk.write(anchor_payload)
    print(f"[DEPLOY] Generated deployable asset: {target_disk}")


if __name__ == "__main__":
    create_deployable_media()