#pragma once
#include <fmt/format.h>
#include <type_traits>

// Just a convenience class to ensure all fmt buffers are nul;-terminated

class StringBuffer {
   fmt::memory_buffer m_buffer;

   // Helper function to check if any argument is a null pointer
   template <typename T>
   static bool is_null_pointer(const T& arg) {
      
      bool rc = false;

      if constexpr (std::is_pointer_v<std::remove_reference_t<T>>) {
         rc = arg == nullptr;
      }

      return rc; // Non-pointer types cannot be null
   }

   template <typename... Args>
   bool has_null_pointers(const Args&... args) {
      return (... || is_null_pointer(args));
   }

public:
   template <typename... Args>
   inline void format(fmt::format_string<Args...> fmt_str, Args&&... args) {
      // Check for null pointers
      if (has_null_pointers(args...)) {
         m_buffer.clear();
         m_buffer.push_back('\0');
         return;
      }
      fmt::format_to(std::back_inserter(m_buffer), fmt_str, std::forward<Args>(args)...);
      m_buffer.push_back('\0');
   }

   template <typename... Args>
   inline void format_(fmt::format_string<Args...> fmt_str, Args&&... args) {
      // Check for null pointers
      if (has_null_pointers(args...)) {
         m_buffer.clear();
         return;
      }
      fmt::format_to(std::back_inserter(m_buffer), fmt_str, std::forward<Args>(args)...);
   }

   inline const char* data() const { return m_buffer.data(); }

   inline void clear() { m_buffer.clear(); }

   inline void push_back(char c) { m_buffer.push_back(c); }
};