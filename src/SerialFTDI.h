#pragma once

#include "serial_interface.h"
#include "error_types.h"
#include <vector>
#include <string>
#include <cstring>
#include <chrono>
#include <thread>

// FTDI D2XX headers
#include <ftd2xx.h>

namespace orcaSDK {

/**
 * @brief SerialInterface implementation using FTDI D2XX library.
 * 
 * This bypasses the OS serial driver to support non-standard baud rates
 * on macOS (and other platforms). Supports any baud rate up to 3,000,000.
 */
class SerialFTDI : public SerialInterface {
public:
    /**
     * @brief Construct SerialFTDI with specified latency timer.
     * @param latency_ms FTDI latency timer in milliseconds (1-255, default 1)
     */
    SerialFTDI(uint8_t latency_ms = 1) : latency_timer_ms(latency_ms) {
        read_buffer.reserve(256);
        send_buffer.reserve(256);
    }

    ~SerialFTDI() {
        close_serial_port();
    }

    OrcaError open_serial_port(int serial_port_number, unsigned int baud) override {
        // For FTDI, port number maps to device index
        FT_STATUS status = FT_Open(serial_port_number, &ft_handle);
        if (status != FT_OK) {
            return { static_cast<int>(status), "Failed to open FTDI device by index: " + std::to_string(status) };
        }
        return configure(baud);
    }

    OrcaError open_serial_port(std::string serial_port_path, unsigned int baud) override {
        // Extract serial number from path
        // Common formats: "/dev/cu.usbserial-ABA76SF6" or just "ABA76SF6"
        std::string serial_number = serial_port_path;
        
        // Extract serial number from typical macOS path
        size_t pos = serial_port_path.find("usbserial-");
        if (pos != std::string::npos) {
            serial_number = serial_port_path.substr(pos + 10);
        }
        
        // Also handle the case where path ends with serial number after last hyphen
        if (serial_number == serial_port_path) {
            pos = serial_port_path.rfind('-');
            if (pos != std::string::npos && pos < serial_port_path.length() - 1) {
                serial_number = serial_port_path.substr(pos + 1);
            }
        }
        
        // Try opening by serial number
        FT_STATUS status = FT_OpenEx(
            const_cast<char*>(serial_number.c_str()),
            FT_OPEN_BY_SERIAL_NUMBER,
            &ft_handle
        );
        
        if (status != FT_OK) {
            // Fallback: try opening first available device
            status = FT_Open(0, &ft_handle);
            if (status != FT_OK) {
                return { static_cast<int>(status), "Failed to open FTDI device. Status: " + std::to_string(status) };
            }
        }
        
        return configure(baud);
    }

    void close_serial_port() override {
        if (port_is_open) {
            FT_Close(ft_handle);
            port_is_open = false;
            ft_handle = nullptr;
        }
    }

    void adjust_baud_rate(uint32_t baud_rate_bps) override {
        if (port_is_open) {
            FT_STATUS status = FT_SetBaudRate(ft_handle, baud_rate_bps);
            if (status == FT_OK) {
                current_baud_rate = baud_rate_bps;
            }
        }
    }

    bool ready_to_send() override {
        return port_is_open;
    }

    void send_byte(uint8_t data) override {
        send_buffer.push_back(data);
    }

    void tx_enable(size_t _bytes_to_read) override {
        if (!port_is_open || send_buffer.empty()) return;

        expected_response_length = _bytes_to_read;
        
        // Clear any pending receive data
        FT_Purge(ft_handle, FT_PURGE_RX);
        read_buffer.clear();

        // Write all buffered data
        DWORD bytes_written = 0;
        FT_STATUS status = FT_Write(ft_handle, send_buffer.data(), 
                                     static_cast<DWORD>(send_buffer.size()), &bytes_written);
        
        send_buffer.clear();
    }

    bool ready_to_receive() override {
        if (!port_is_open) return false;

        // Check if we have buffered data
        if (!read_buffer.empty()) return true;

        // Check FTDI for available data
        DWORD rx_bytes = 0;
        DWORD tx_bytes = 0;
        DWORD event_status = 0;
        
        FT_GetStatus(ft_handle, &rx_bytes, &tx_bytes, &event_status);
        
        if (rx_bytes > 0) {
            // Read available data into buffer
            std::vector<uint8_t> temp_buffer(rx_bytes);
            DWORD bytes_read = 0;
            FT_Read(ft_handle, temp_buffer.data(), rx_bytes, &bytes_read);
            
            for (DWORD i = 0; i < bytes_read; i++) {
                read_buffer.push_back(temp_buffer[i]);
            }
        }

        return !read_buffer.empty();
    }

    uint8_t receive_byte() override {
        if (read_buffer.empty()) return 0;
        
        uint8_t byte = read_buffer.front();
        read_buffer.erase(read_buffer.begin());
        return byte;
    }

    OrcaResult<std::vector<uint8_t>> receive_bytes_blocking() override {
        if (!port_is_open) {
            return { {}, { 1, "No serial port open." } };
        }

        std::vector<uint8_t> response;
        
        // Start with any data already in our buffer
        while (!read_buffer.empty() && response.size() < expected_response_length) {
            response.push_back(read_buffer.front());
            read_buffer.erase(read_buffer.begin());
        }
        
        // Calculate timeout based on expected bytes and baud rate
        int timeout_ms = 50; // Base timeout
        if (expected_response_length > 0 && current_baud_rate > 0) {
            // Time for bytes (11 bits per byte with parity) + margin
            int byte_time_ms = static_cast<int>((expected_response_length * 11 * 1000) / current_baud_rate);
            timeout_ms = std::max(timeout_ms, byte_time_ms + 25);
        }

        auto start_time = std::chrono::steady_clock::now();
        auto timeout_duration = std::chrono::milliseconds(timeout_ms);

        while (response.size() < expected_response_length) {
            // Check for timeout
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            if (elapsed > timeout_duration) {
                return { response, { 1, "Read blocking timed out." } };
            }

            // Check for available data
            DWORD rx_bytes = 0;
            DWORD tx_bytes = 0;
            DWORD event_status = 0;
            
            FT_GetStatus(ft_handle, &rx_bytes, &tx_bytes, &event_status);
            
            if (rx_bytes > 0) {
                DWORD to_read = std::min(rx_bytes, 
                    static_cast<DWORD>(expected_response_length - response.size()));
                std::vector<uint8_t> temp_buffer(to_read);
                DWORD bytes_read = 0;
                
                FT_Read(ft_handle, temp_buffer.data(), to_read, &bytes_read);
                
                for (DWORD i = 0; i < bytes_read; i++) {
                    response.push_back(temp_buffer[i]);
                }
            } else {
                // Small sleep to prevent busy-waiting
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }

        return { response, { 0, "" } };
    }

    void flush_and_discard_receive_buffer() override {
        if (port_is_open) {
            FT_Purge(ft_handle, FT_PURGE_RX);
            read_buffer.clear();
        }
    }

    bool is_open() override {
        return port_is_open;
    }

    /**
     * @brief Set the FTDI latency timer.
     * @param latency_ms Latency in milliseconds (1-255)
     */
    void set_latency_timer(uint8_t latency_ms) {
        latency_timer_ms = latency_ms;
        if (port_is_open) {
            FT_SetLatencyTimer(ft_handle, latency_timer_ms);
        }
    }

    /**
     * @brief Get current latency timer setting.
     * @return Current latency timer in milliseconds
     */
    uint8_t get_latency_timer() const {
        return latency_timer_ms;
    }

private:
    FT_HANDLE ft_handle = nullptr;
    bool port_is_open = false;
    uint8_t latency_timer_ms = 1;
    uint32_t current_baud_rate = 19200;
    size_t expected_response_length = 0;

    std::vector<uint8_t> send_buffer;
    std::vector<uint8_t> read_buffer;

    OrcaError configure(unsigned int baud) {
        FT_STATUS status;

        // Reset the device
        status = FT_ResetDevice(ft_handle);
        if (status != FT_OK) {
            FT_Close(ft_handle);
            return { static_cast<int>(status), "Failed to reset device" };
        }

        // Set baud rate - D2XX accepts any value!
        status = FT_SetBaudRate(ft_handle, baud);
        if (status != FT_OK) {
            FT_Close(ft_handle);
            return { static_cast<int>(status), "Failed to set baud rate: " + std::to_string(baud) };
        }
        current_baud_rate = baud;

        // Set data characteristics: 8 bits, 1 stop bit, even parity (per ORCA spec)
        status = FT_SetDataCharacteristics(ft_handle, FT_BITS_8, FT_STOP_BITS_1, FT_PARITY_EVEN);
        if (status != FT_OK) {
            FT_Close(ft_handle);
            return { static_cast<int>(status), "Failed to set data characteristics" };
        }

        // Set flow control (none for ORCA)
        status = FT_SetFlowControl(ft_handle, FT_FLOW_NONE, 0, 0);
        if (status != FT_OK) {
            FT_Close(ft_handle);
            return { static_cast<int>(status), "Failed to set flow control" };
        }

        // Set latency timer (critical for performance!)
        status = FT_SetLatencyTimer(ft_handle, latency_timer_ms);
        if (status != FT_OK) {
            FT_Close(ft_handle);
            return { static_cast<int>(status), "Failed to set latency timer" };
        }

        // Set timeouts (read, write in milliseconds)
        status = FT_SetTimeouts(ft_handle, 100, 100);
        if (status != FT_OK) {
            FT_Close(ft_handle);
            return { static_cast<int>(status), "Failed to set timeouts" };
        }

        // Set USB transfer sizes for better performance
        status = FT_SetUSBParameters(ft_handle, 4096, 4096);
        if (status != FT_OK) {
            // Non-fatal, continue anyway
        }

        // Purge any existing data in buffers
        FT_Purge(ft_handle, FT_PURGE_RX | FT_PURGE_TX);

        port_is_open = true;
        return { 0 };
    }
};

} // namespace orcaSDK