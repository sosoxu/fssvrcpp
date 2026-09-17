// wire_shapes 实现（从 REST DTO 抽出，两条协议共用）。
#include "app/usecases/wire_shapes.h"

#include "common/time/time_format.h"

namespace fss::app {

std::string FileNameOfPath(std::string_view path) {
  const auto slash = path.rfind('/');
  return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

std::string FileSourceFromRecordNode(const json::Value& node) {
  if (node.is_string()) return node.get<std::string>();
  if (!node.is_object()) return {};
  if (const auto top = node.find("FileSource"); top != node.end() && top->is_string()) {
    return top->get<std::string>();
  }
  const auto data = node.find("data");
  if (data == node.end()) return {};
  const auto props = data->find("DatasetProperties");
  if (props == data->end()) return {};
  const auto info = props->find("FileSourceInfo");
  if (info == props->end()) return {};
  const auto source = info->find("FileSource");
  if (source == info->end() || !source->is_string()) return {};
  return source->get<std::string>();
}

json::Value DmsLocationJson(std::string_view signed_url, std::string_view source,
                            std::string_view created_by, std::int64_t expires_at_epoch_seconds,
                            bool collection, int file_count,
                            const std::vector<std::string>& file_names) {
  json::Value location = json::Value::object();
  location["signedUrl"] = std::string(signed_url);
  location[collection ? "fileCollectionSource" : "fileSource"] = std::string(source);
  if (collection) {
    //  本项目一个指令 = 一个对象：调用方没给名字时按 source 的最后一段推导
    std::vector<std::string> names = file_names;
    if (names.empty() && !source.empty()) names.push_back(FileNameOfPath(source));
    json::Value array = json::Value::array();
    for (const auto& name : names) array.push_back(name);
    location["fileCount"] = names.empty() ? file_count : static_cast<int>(names.size());
    location["fileNames"] = std::move(array);
  }
  location["createdBy"] = std::string(created_by);
  location["expiryTime"] = time::ToOsduTimestamp(expires_at_epoch_seconds, 0);
  return location;
}

}  // namespace fss::app
