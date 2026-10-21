#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sparenode/authentication/credential_file_provisioner.hpp"
#include "sparenode/authentication/file_credential_provider.hpp"
#include "sparenode/authentication/sensitive_password.hpp"
#include "sparenode/authentication/sodium_password_verifier.hpp"

namespace
{

/// @brief Restores terminal echo on every password-input exit path.
class ConsoleEchoGuard final
{
  public:
    ConsoleEchoGuard() = default;
    ~ConsoleEchoGuard()
    {
        restore();
    }

    ConsoleEchoGuard(const ConsoleEchoGuard &) = delete;
    ConsoleEchoGuard &operator=(const ConsoleEchoGuard &) = delete;
    ConsoleEchoGuard(ConsoleEchoGuard &&) = delete;
    ConsoleEchoGuard &operator=(ConsoleEchoGuard &&) = delete;

    /// @brief Disables echo only when standard input is an interactive terminal.
    /// @return `true` after saving state and disabling echo.
    [[nodiscard]] bool disable() noexcept
    {
#ifdef _WIN32
        input_ = GetStdHandle(STD_INPUT_HANDLE);
        if (input_ == nullptr || input_ == INVALID_HANDLE_VALUE ||
            GetConsoleMode(input_, &original_mode_) == FALSE)
        {
            return false;
        }
        active_ = SetConsoleMode(input_, original_mode_ & ~ENABLE_ECHO_INPUT) != FALSE;
#else
        if (::isatty(STDIN_FILENO) == 0 || ::tcgetattr(STDIN_FILENO, &original_mode_) != 0)
        {
            return false;
        }
        auto hidden_mode = original_mode_;
        hidden_mode.c_lflag &= static_cast<tcflag_t>(~ECHO);
        active_ = ::tcsetattr(STDIN_FILENO, TCSANOW, &hidden_mode) == 0;
#endif
        return active_;
    }

  private:
    /// @brief Restores the saved console mode once after a successful disable.
    void restore() noexcept
    {
        if (!active_)
        {
            return;
        }
#ifdef _WIN32
        SetConsoleMode(input_, original_mode_);
#else
        ::tcsetattr(STDIN_FILENO, TCSANOW, &original_mode_);
#endif
        active_ = false;
    }

#ifdef _WIN32
    HANDLE input_{INVALID_HANDLE_VALUE}; ///< Interactive standard-input console.
    DWORD original_mode_{};              ///< Console flags restored after the prompt.
#else
    termios original_mode_{}; ///< Terminal flags restored after the prompt.
#endif
    bool active_{}; ///< Indicates that echo was successfully disabled.
};

/// @brief Owns a growing plaintext input buffer and erases it on every exit path.
class SensitiveInputBuffer final
{
  public:
    SensitiveInputBuffer()
    {
        bytes_.reserve(sparenode::authentication::maximum_password_bytes + 1);
    }

    ~SensitiveInputBuffer()
    {
        sparenode::authentication::erase_sensitive_bytes(bytes_);
    }

    SensitiveInputBuffer(const SensitiveInputBuffer &) = delete;
    SensitiveInputBuffer &operator=(const SensitiveInputBuffer &) = delete;

    /// @brief Appends one byte while retaining a single fixed-capacity allocation.
    /// @param[in] byte Password byte read from the terminal.
    void push_back(const char byte)
    {
        bytes_.push_back(byte);
    }

    /// @brief Reports the current number of plaintext bytes.
    [[nodiscard]] std::size_t size() const noexcept
    {
        return bytes_.size();
    }

    /// @brief Transfers the erasable allocation to the authentication domain owner.
    [[nodiscard]] sparenode::authentication::SensitivePassword release() noexcept
    {
        return sparenode::authentication::SensitivePassword(std::move(bytes_));
    }

  private:
    std::vector<char> bytes_; ///< Single erasable allocation for one console entry.
};

/// @brief Categorizes safe interactive password-input failures.
enum class PasswordInputError : std::uint8_t
{
    console_unavailable, ///< Standard input is not an interactive console.
    read_failed,         ///< The console stopped before completing an input line.
    password_too_large,  ///< Input exceeded the authentication boundary.
};

/// @brief Reads one hidden bounded password from an interactive console.
[[nodiscard]] sparenode::Result<sparenode::authentication::SensitivePassword, PasswordInputError>
read_password(const std::string_view prompt)
{
    std::cout << prompt << std::flush;
    ConsoleEchoGuard echo;
    if (!echo.disable())
    {
        std::cout << '\n';
        return sparenode::unexpected(PasswordInputError::console_unavailable);
    }

    SensitiveInputBuffer input;
    char byte = '\0';
    while (std::cin.get(byte) && byte != '\n')
    {
        if (input.size() > sparenode::authentication::maximum_password_bytes)
        {
            continue;
        }
        input.push_back(byte);
    }
    std::cout << '\n';
    if (!std::cin && !std::cin.eof())
    {
        return sparenode::unexpected(PasswordInputError::read_failed);
    }
    if (input.size() > sparenode::authentication::maximum_password_bytes)
    {
        return sparenode::unexpected(PasswordInputError::password_too_large);
    }
    return input.release();
}

/// @brief Returns a non-sensitive interactive input diagnostic.
[[nodiscard]] const char *to_string(const PasswordInputError error) noexcept
{
    switch (error)
    {
    case PasswordInputError::console_unavailable:
        return "password input requires an interactive terminal";
    case PasswordInputError::read_failed:
        return "unable to read password input";
    case PasswordInputError::password_too_large:
        return "password exceeds the supported size limit";
    }
    return "unknown password input error";
}

/// @brief Parses the optional output selection without accepting password arguments.
[[nodiscard]] std::optional<std::filesystem::path> parse_output_path(const int argc,
                                                                     const char *const *argv)
{
    if (argc == 1)
    {
        return std::filesystem::path("credentials.conf");
    }
    if (argc == 2 && std::string_view(argv[1]) == "--help")
    {
        return std::nullopt;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--output" && !std::string_view(argv[2]).empty())
    {
        return std::filesystem::path(argv[2]);
    }
    return std::nullopt;
}

/// @brief Prints the only supported non-secret command-line interface.
void print_usage()
{
    std::cout << "usage: sparenode-credentials [--output <new-credentials-file>]\n";
}

} // namespace

int main(const int argc, const char *const argv[])
{
    const auto output_path = parse_output_path(argc, argv);
    if (!output_path.has_value())
    {
        print_usage();
        return argc == 2 && std::string_view(argv[1]) == "--help" ? 0 : 2;
    }

    const auto verifier = sparenode::authentication::SodiumPasswordVerifier::create();
    if (!verifier)
    {
        std::cerr << "error: password cryptography could not be initialized\n";
        return 3;
    }

    std::cout << "Login: " << std::flush;
    std::string login;
    if (!std::getline(std::cin, login))
    {
        std::cerr << "error: unable to read login\n";
        return 2;
    }
    if (!sparenode::authentication::is_valid_owner_login(login))
    {
        std::cerr << "error: login is empty, oversized, or contains control bytes\n";
        return 2;
    }

    auto password = read_password("Password: ");
    if (!password)
    {
        std::cerr << "error: " << to_string(password.error()) << '\n';
        return 2;
    }
    auto confirmation = read_password("Confirm password: ");
    if (!confirmation)
    {
        std::cerr << "error: " << to_string(confirmation.error()) << '\n';
        return 2;
    }
    if (!password->equals(confirmation.value()))
    {
        std::cerr << "error: password confirmation does not match\n";
        return 2;
    }

    auto result = sparenode::authentication::CredentialFileProvisioner::provision(
        *output_path, login, std::move(password).value(), verifier.value());
    if (!result)
    {
        std::cerr << "error: " << sparenode::authentication::to_string(result.error()) << '\n';
        return 3;
    }

    std::cout << "Credential file created.\n";
    return 0;
}
