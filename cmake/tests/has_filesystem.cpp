#include <filesystem>

int main()
{
  const auto p = std::filesystem::current_path();
  return std::filesystem::exists(p) ? 0 : 1;
}
