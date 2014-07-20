// To compile:
// g++ extract_files.c -I/usr/include/libusb-1.0/ -lusb /usr/lib64/libusb-1.0.so --std=c++11

#include <libusb.h>
#include <getopt.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <string>
#include <vector>

using namespace std;

static void HexDump(const uint8_t* data, int size) {
  for (int i = 0; i < size; ++i) {
    printf(" %02X", data[i]);
    if (i % 32 == 31) {
      printf("\n");
    }
  }
  if (size % 32 != 0) {
    printf("\n");
  }
}

void HexDump(const vector<uint8_t>& data) {
  HexDump(&data[0], data.size());
}

struct TomTomFile {
  uint32_t id;
  uint32_t length;
};

// Opens a HID USB device and cleans everything in its destructor.
// Call UsbDevice::Shutdow before leaving the application.
class UsbDevice {
public:
  // Open the first device matching vid and pid.
  bool Open(int vid, int pid) {
    if (device_ != nullptr) {
      return false;
    }
    if (context_ == nullptr) {
      int r = libusb_init(&context_); // initialize a library session
      if (r < 0) {
        fprintf(stderr, "Failed to initialize the usb library, error: %i\n",
               r);
        return false;
      }
      libusb_set_debug(context_, 3);
    }
    device_ = libusb_open_device_with_vid_pid(context_, vid, pid);
    if (device_ == nullptr) {
      fprintf(stderr, "Failed to open the device %X, %X\n", vid, pid);
      return false;
    }

    if (libusb_kernel_driver_active(device_, 0)) {
      int res = libusb_detach_kernel_driver(device_, 0);
      if (res == 0) {
        reattach_kernel_driver_ = true;
      } else {
        fprintf(stderr, "Error detaching the kernel driver.\n");
        return false;
      }
    }
    int res = libusb_claim_interface(device_, 0);
    if (res != 0) {
      fprintf(stderr, "Error claiming the interface. Error %i\n", res);
      return false;
    }
    release_interface_ = true;
    return true;
  }

  ~UsbDevice() {
    int res;
    if (release_interface_) {
      res = libusb_release_interface(device_, 0);
      if (res != 0) {
        fprintf(stderr, "Error releasing the interface.\n");
      }
    }
    if (reattach_kernel_driver_) {
      res = libusb_attach_kernel_driver(device_, 0);
      if (res != 0) {
        fprintf(stderr, "Failed to reattach the kernel driver\n");
      }
    }
    if (device_) {
      libusb_close(device_);
    }
  }

  // Call Shutdow before exiting the application.
  static void Shutdown() {
    if (context_ != nullptr) {
      libusb_exit(context_);
    }
    context_ = nullptr;
  }

  libusb_device_handle* device() { return device_; }

private:
  libusb_device_handle* device_ = nullptr;
  bool reattach_kernel_driver_ = false;
  bool release_interface_ = false;
  static libusb_context* context_;
};

libusb_context* UsbDevice::context_ = nullptr;

class TomTomWatch : public UsbDevice {
 public:
  // Tomtom command format:
  // Write to endpoint 5:
  // 09 + number of actual bytes in the command + Counter + Command bytes
  // And read from endpoint 0x84:
  // 01 + number of bytes in the reply + Counter + Reply bytes.
  // There is probably a status somewhere in the reply but I did not find it.

  // Send a command and read the response.
  // cmd: byte 0 is the length.
  bool SendCommand(const vector<uint8_t> command, vector<uint8_t>* reply) {
    // 64 bytes is the maximum payload size for HID USB 1.1 devices.
    uint8_t data[64];
    if (command.size() > 60) {
      return false;
    }
    memset(data, 0, sizeof(data));
    data[0] = 9;
    data[1] = command.size();
    data[2] = ++counter_;
    memcpy(data + 3, &command[0], command.size());
    // Use endpoint 0x05 to send the command.
    int transferred = 0;
    int status =
        libusb_interrupt_transfer(device(), 0x5, data, 3 + command.size(),
                                  &transferred, 10000 /* 10 seconds timeout */);
    if (status != 0 || transferred != 3 + data[1]) {
      printf("Out transfer failed: %i %s\n", status, libusb_error_name(status));
      return false;
    }
    // Use endpoint 0x84 to read the reply.
    status = libusb_interrupt_transfer(device(), 0x84, data, 64, &transferred,
                                       10000 /* 10 seconds */);
    if (status != 0) {
      printf("IN transfer failed: %i %s\n", status, libusb_error_name(status));
      return false;
    }
    if (transferred < 2 || data[1] > transferred + 3 || data[0] != 1 ||
        data[2] != counter_) {
      printf("Invalid reply received\n");
      HexDump(data, transferred);
      return false;
    } else {
      reply->assign(data + 3, data + 3 + data[1]);
      return true;
    }
  }

  bool ReadDirectory(vector<TomTomFile>* files) {
    // This command initiates the file enumeration.
    static const vector<uint8_t> ReadFirst = {0x11, 0x00, 0x00, 0x00, 0x00,
                                              0x00, 0x00, 0x00, 0x00, 0x00};
    // And this command is called repeatedly until byte 20 is 1.
    static const vector<uint8_t> ReadNext = {0x12, 0x00};
    files->clear();
    vector<uint8_t> reply;
    if (!SendCommand(ReadFirst, &reply)) {
      return false;
    }
    // Avoid an infinite loop if things go wrong.
    for (int i = 0; i < 1000; ++i) {
      if (!SendCommand(ReadNext, &reply) || reply.size() != 22) {
        fprintf(stderr, "Read directory failed\n");
        HexDump(reply);
        return false;
      }
      // This byte seems to indicate the last entry in the directory.
      if (reply[20] == 1) {
        return true;
      }
      TomTomFile file;

      file.length = (reply[13] << 24) + (reply[14] << 16) +
          (reply[15] << 8) + reply[16];
      file.id = (reply[6] << 24) + (reply[7] << 16) +
          (reply[8] << 8) + reply[9];
      files->push_back(file);
    }
    return true;
  }

  // It is important to call CloseFile after reading a file, otherwise errors
  // will happen after reading a few files.
  bool ReadFile(const TomTomFile& from, vector<uint8_t>* data) {
    // I am not sure why two commands are needed before reading a file.
    vector<uint8_t> open_file1 = {0x06, 0x00, 0x91, 0x00, 0x01, 0x00};
    vector<uint8_t> open_file2 = {0x05, 0x00, 0x91, 0x00, 0x01, 0x00};
    // This function needs to be called repeatedly until the file ends.
    vector<uint8_t> read_command = {0x07, 0x00, 0x00, 0x00, 0x00,
                                    0x00, 0x00, 0x00, 0x32, 0x00};
    const uint32_t kMaxReadSize = 0x32;
    open_file1[2] = open_file2[2] = read_command[2] = from.id >> 24;
    open_file1[3] = open_file2[3] = read_command[3] = (from.id >> 16) & 0xFF;
    open_file1[4] = open_file2[4] = read_command[4] = (from.id >>  8) & 0xFF;
    open_file1[5] = open_file2[5] = read_command[5] = from.id & 0xFF;
    vector<uint8_t> reply;
    if (!SendCommand(open_file1, &reply)) {
      fprintf(stderr, "Read init command  1 failed\n");
      return false;
    }
    if (!SendCommand(open_file2, &reply)) {
      fprintf(stderr, "Read init command  1 failed\n");
      return false;
    }
    data->reserve(from.length);
    for (uint32_t pos = 0; pos < from.length; pos += kMaxReadSize) {
      int l = min(kMaxReadSize, from.length - pos);
      read_command[7] = l & 0xFF;
      if (!SendCommand(read_command, &reply)) {
        fprintf(stderr, "Read command failed\n");
        return false;
      }
      if (reply.size() < 9 || reply[8] != l) {
        fprintf(
            stderr,
            "Unexpected read reply at offset %i, tried to read 0x%x bytes:\n",
            pos, l);
        HexDump(reply);
        return false;
      }
      data->insert(data->end(), reply.begin() + 9, reply.end());
    }
    return true;
  }
  // Close a previously open file.
  bool CloseFile(uint32_t id) {
    vector<uint8_t> close_file = {0x0C, 0x00, 0x00, 0x00, 0x00, 0x00};
    close_file[2] = id >> 24;
    close_file[3] = (id >> 16) & 0xFF;
    close_file[4] = (id >>  8) & 0xFF;
    close_file[5] = id & 0xFF;
    vector<uint8_t> reply;
    if (!SendCommand(close_file, &reply)) {
      fprintf(stderr, "Failed to close the file %X\n", id);
      return false;
    }
    return true;
  }

  bool debug_ = false;
  uint8_t counter_ = 0;
};

bool ReadFiles(uint16_t vid, uint16_t pid, bool extract_all) {
  TomTomWatch device;
  if (!device.Open(vid, pid)) {
    printf("Failed to access the watch.\n");
    return false;
  }
  unsigned char buffer[256];
  int length;
  length = libusb_get_string_descriptor_ascii(device.device(), 2, buffer,
                                              sizeof(buffer));
  if (length > 0) {
    printf("Device: %s\n", buffer);
  }
  length = libusb_get_string_descriptor_ascii(device.device(), 3, buffer,
                                              sizeof(buffer));
  if (length > 0) {
    printf("Serial number: %s\n", buffer);
  }
  vector<TomTomFile> files;
  if (!device.ReadDirectory(&files)) {
    printf("Failed to read the file directory.\n");
    return false;
  }

  for (const auto& file : files) {
    // The 0x91... files seem to be the ones containing the run/cycle/swim track
    // files.
    if (!extract_all && ((file.id & 0xFF000000) != 0x91000000)) {
      continue;
    }
    printf("Reading file: %X length %i\n", file.id, file.length);
    vector<uint8_t> data;
    if (!device.ReadFile(file, &data)) {
      printf("Failed to read file %X\n", file.id);
      return false;
    }
    device.CloseFile(file.id);
    char name[64];
    if ((file.id & 0xFF000000) == 0x91000000 && data.size() > 100 &&
        data[0] == 0x20 && data[1] == 0x05) {
      // A track file.
      time_t tt =
          (data[11] << 24) + (data[10] << 16) + (data[9] << 8) + data[8];
      if (strftime(name, sizeof(name), "%F_%T.ttbin", gmtime(&tt)) <= 0) {
        // Fallback to using the id as the file name
        snprintf(name, sizeof(name), "%X.bin", file.id);
      }
    } else {
      // All other files.
      snprintf(name, sizeof(name), "%X.bin", file.id);
    }
    printf("Writing to disk: %s\n", name);
    FILE* fout = fopen(name, "w");
    if (fout == nullptr) {
      fprintf(stderr, "Failed to open %s\n", name);
      perror("Error: ");
      return false;
    }
    if (fwrite(&data[0], 1, data.size(), fout) != data.size()) {
      fprintf(stderr, "Failed to write to %s\n", name);
      perror("Error: ");
      fclose(fout);
      return false;
    }
    fclose(fout);
  }
  return true;
}

int main(int argc, char** argv) {
  int transfer_all_files = 0;
  // Default VID and PID for the Tomtom multisport cardio watch.
  uint16_t vendor_id = 0x1390;
  uint16_t product_id = 0x7474;
  struct option long_options[] = {
      /* These options set a flag. */
      {"all", no_argument, &transfer_all_files, 1},
      {"vid", required_argument, nullptr, 'v'},
      {"pid", required_argument, nullptr, 'p'},
      {0, 0, 0, 0}};

  int long_index = 0;
  int opt;
  while ((opt = getopt_long(argc, argv,"",
                            long_options, &long_index )) != -1) {
    switch (opt) {
      case 'v':
        vendor_id = stoul(optarg, 0, 0);
        break;
      case 'p':
        product_id = stoul(optarg, 0, 0);
        break;
      case 0:
        break;
      default:
        printf("%i %c\n", opt, opt);
        printf(
            "extract_files\n --vid to specify the vendor id\n"
            " --pid to specify the product id\n"
            " --all to extract all the files\n");
        return -1;
    }
  }
  ReadFiles(vendor_id, product_id, false);
  UsbDevice::Shutdown();
  return 0;
}
