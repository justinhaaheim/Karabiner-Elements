#pragma once

#include "file_monitor.hpp"
#include "filesystem.hpp"
#include "spdlog_utility.hpp"
#include <fstream>
#include <vector>
#include <spdlog/spdlog.h>
#include <thread>

class log_monitor final {
public:
  typedef std::function<void(const std::string& line)> new_log_line_callback;

  log_monitor(const log_monitor&) = delete;

  // FSEvents (file_monitor) notifies file changes only when the target file is just closed.
  // So, it is not usable for log_monitor since spdlog keeps opening log files and appending lines.
  //
  // We use timer to observe file changes instead.

  log_monitor(spdlog::logger& logger,
              const std::vector<std::string>& targets,
              const new_log_line_callback& callback) : logger_(logger),
                                                       callback_(callback),
                                                       queue_(dispatch_queue_create(nullptr, nullptr)) {
    // ----------------------------------------
    // setup initial_lines_

    for (const auto& target : targets) {
      add_initial_lines(target + ".1.txt");
      add_initial_lines(target + ".txt");

      files_.push_back(target + ".txt");
    }

    // ----------------------------------------
    // setup timer_

    timer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue_);
    if (!timer_) {
      logger_.error("failed to dispatch_source_create @ {0}", __PRETTY_FUNCTION__);
    } else {
      dispatch_source_set_timer(timer_, dispatch_time(DISPATCH_TIME_NOW, 1.0 * NSEC_PER_SEC), 1.0 * NSEC_PER_SEC, 0);
      dispatch_source_set_event_handler(timer_, ^{
        for (const auto& file : files_) {
          if (auto size = filesystem::file_size(file)) {
            auto it = read_position_.find(file);
            if (it != read_position_.end()) {
              if (it->second != *size) {
                call_callback(file);
              }
            }
          }
        }
      });
      dispatch_resume(timer_);
    }
  }

  ~log_monitor(void) {
    if (timer_) {
      dispatch_source_cancel(timer_);
      dispatch_release(timer_);
      timer_ = 0;
    }

    dispatch_release(queue_);
  }

  const std::vector<std::pair<uint64_t, std::string>>& get_initial_lines(void) const {
    return initial_lines_;
  }

private:
  void add_initial_lines(const std::string& file_path) {
    std::ifstream stream(file_path);
    std::string line;
    std::streampos read_position;

    while (std::getline(stream, line)) {
      if (add_initial_line(line)) {
        const size_t max_initial_lines = 250;
        while (initial_lines_.size() > max_initial_lines) {
          initial_lines_.erase(initial_lines_.begin());
        }
      }
      read_position = stream.tellg();
    }

    read_position_[file_path] = read_position;
  }

  bool add_initial_line(const std::string& line) {
    if (auto sort_key = spdlog_utility::get_sort_key(line)) {
      if (initial_lines_.empty()) {
        initial_lines_.push_back(std::make_pair(*sort_key, line));
        return true;
      }

      if (*sort_key < initial_lines_.front().first) {
        // line is too old
        return false;
      }

      if (*sort_key > initial_lines_.back().first) {
        initial_lines_.push_back(std::make_pair(*sort_key, line));
        return true;
      }

      for (auto it = initial_lines_.begin(); it != initial_lines_.end(); ++it) {
        if (*sort_key < it->first) {
          initial_lines_.insert(it, std::make_pair(*sort_key, line));
          return true;
        }
      }

      initial_lines_.push_back(std::make_pair(*sort_key, line));
      return true;
    }

    return false;
  }

  void call_callback(const std::string& file_path) {
    std::ifstream stream(file_path);
    if (!stream) {
      return;
    }

    // ----------------------------------------
    // seek

    auto it = read_position_.find(file_path);
    if (it != read_position_.end()) {
      if (auto size = filesystem::file_size(file_path)) {
        if (it->second < *size) {
          stream.seekg(it->second);
        }
      }
    }

    // ----------------------------------------
    // read

    std::string line;
    std::streampos read_position;
    while (std::getline(stream, line)) {
      if (callback_) {
        callback_(line);
      }
      read_position = stream.tellg();
    }

    read_position_[file_path] = read_position;
  }

  spdlog::logger& logger_;
  new_log_line_callback callback_;

  dispatch_queue_t queue_;
  dispatch_source_t timer_;

  std::vector<std::pair<uint64_t, std::string>> initial_lines_;
  std::unordered_map<std::string, std::streampos> read_position_;
  std::vector<std::string> files_;
};