# =========================================
# 📘 SNAPSHOTTER KERNEL MODULE SETUP GUIDE
# =========================================

# --- STEP 1: Install Required Packages ---
sudo apt update && sudo apt install -y build-essential linux-headers-$(uname -r)

# --- STEP 2: Clean and Build Kernel Module ---
cd ~/Desktop/snapshotter/module
make clean && make

# --- STEP 3: Remove Existing Module (if any) ---
sudo rmmod snapshot_module 2>/dev/null || true

# --- STEP 4: Remove Old Device Node ---
sudo rm -f /dev/snapshotctl

# --- STEP 5: Insert the Newly Built Module ---
sudo insmod snapshot_module.ko

# --- STEP 6: Verify Module is Loaded ---
lsmod | grep snapshot_module || echo "Module not loaded"
sudo dmesg | tail -n 10

# Find Major Number from above logs
eg. major in 240, major in 244

# --- STEP 8: Create New Device Node ---
sudo mknod /dev/snapshotctl c (Major number) 0
sudo chmod 666 /dev/snapshotctl

# --- STEP 9: Verify Device Node ---
ls -l /dev/snapshotctl

# --- STEP 10: Compile User-space Tools ---
cd ~/Desktop/snapshotter/user
make
gcc testprog.c -o testprog

# --- STEP 11: Run Snapshot Controller ---
sudo ./snapshotctl

# --- STEP 12: (Optional) Run Test Program ---
./testprog

# --- STEP 13: Unload Module & Cleanup ---
sudo rmmod snapshot_module
sudo rm -f /dev/snapshotctl
make clean
echo "✅ Snapshotter module unloaded and cleaned successfully!"

