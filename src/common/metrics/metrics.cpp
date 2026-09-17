// MetricsRegistry 实现。设计理由见头文件。
#include "common/metrics/metrics.h"

#include <algorithm>
#include <sstream>

namespace fss::metrics {

namespace {

void EscapeInto(std::string& out, std::string_view value) {
  for (const char c : value) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      default: out.push_back(c);
    }
  }
}

std::string RenderLabels(const Labels& labels) {
  if (labels.empty()) return "";
  std::string out = "{";
  bool first = true;
  for (const auto& [key, value] : labels) {
    if (!first) out += ",";
    first = false;
    out += key;
    out += "=\"";
    EscapeInto(out, value);
    out += "\"";
  }
  out += "}";
  return out;
}

std::string FormatValue(std::int64_t value) { return std::to_string(value); }

}  // namespace

std::string EscapeLabelValue(std::string_view value) {
  std::string out;
  EscapeInto(out, value);
  return out;
}

std::string Registry::SampleKey(std::string_view name, const Labels& labels) {
  std::string key(name);
  for (const auto& [k, v] : labels) {
    key += '\x1f';
    key += k;
    key += '=';
    key += v;
  }
  return key;
}

void Registry::Register(std::string_view name, Kind kind, std::string_view help) {
  std::lock_guard<std::mutex> lock(mutex_);
  families_[std::string(name)] = Family{kind, std::string(help)};
}

void Registry::Increment(std::string_view name, const Labels& labels, std::int64_t delta) {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string key = SampleKey(name, labels);
  samples_[key].value += delta;
  sample_index_[key] = {std::string(name), labels};
}

void Registry::SetGauge(std::string_view name, std::int64_t value, const Labels& labels) {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string key = SampleKey(name, labels);
  samples_[key].value = value;
  sample_index_[key] = {std::string(name), labels};
}

std::int64_t Registry::Value(std::string_view name, const Labels& labels) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = samples_.find(SampleKey(name, labels));
  return it == samples_.end() ? 0 : it->second.value;
}

std::string Registry::RenderPrometheus() const {
  std::lock_guard<std::mutex> lock(mutex_);
  //  按 (name, labels) 排序输出：测试可以逐行断言，抓取器也不在意顺序
  std::map<std::string, std::vector<std::string>> by_family;
  for (const auto& [key, index] : sample_index_) {
    const auto& [name, labels] = index;
    const auto sample = samples_.find(key);
    if (sample == samples_.end()) continue;
    by_family[name].push_back(name + RenderLabels(labels) + " " + FormatValue(sample->second.value));
  }

  std::string out;
  for (auto& [name, lines] : by_family) {
    const auto family = families_.find(name);
    if (family != families_.end()) {
      out += "# HELP " + name + " " + family->second.help + "\n";
      out += "# TYPE " + name + " " +
             (family->second.kind == Kind::kCounter ? "counter" : "gauge") + "\n";
    }
    std::sort(lines.begin(), lines.end());
    for (const auto& line : lines) out += line + "\n";
  }
  return out;
}

void Registry::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  samples_.clear();
  sample_index_.clear();
}

}  // namespace fss::metrics
