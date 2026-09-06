#include <Client/ClientApplicationBase.h>
#include <Client/ClientApplicationBaseParser.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>


namespace po = boost::program_options;


namespace DB
{

/**
 * Program options parsing is very slow in debug builds and it affects .sh tests
 * causing them to timeout sporadically.
 * It seems impossible to enable optimizations for a single function (only to disable them), so
 * instead we extract the code to a separate source file and compile it with different options.
 */
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int UNRECOGNIZED_ARGUMENTS;
}

namespace
{

struct ParsedOptions
{
    po::parsed_options options;
    std::vector<const OptionsAliasParser::Option *> descriptors;
};

ParsedOptions
parseOptions(const std::vector<std::string> & arguments, const po::options_description & description, const OptionsAliasParser & aliases)
{
    ParsedOptions result{po::parsed_options(&description, po::command_line_style::allow_long), {}};
    result.options.options.reserve(arguments.size());
    result.descriptors.reserve(arguments.size());

    size_t argument_index = !arguments.empty() && arguments.front().empty() ? 1 : 0;
    int position_index = 0;
    std::string option_name;
    std::string original_token;

    const auto append_positional = [&](const std::string & argument)
    {
        po::option option;
        option.position_key = position_index++;
        option.value.push_back(argument);
        option.original_tokens.push_back(argument);
        result.options.options.push_back(std::move(option));
        result.descriptors.push_back(nullptr);
    };

    const auto append_option = [&](po::option option, const OptionsAliasParser::Option * descriptor)
    {
        if (descriptor)
        {
            option.string_key = (*descriptor)->key(option.string_key);
            option_name = option.string_key;
            const auto semantic = (*descriptor)->semantic();
            const auto min_tokens = semantic->min_tokens();
            const auto max_tokens = semantic->max_tokens();
            if (option.value.size() > max_tokens)
                throw po::invalid_command_line_syntax(po::invalid_syntax::extra_parameter);

            while (option.value.size() < max_tokens && argument_index < arguments.size())
            {
                const auto & value = arguments[argument_index];
                if (value.size() > 1 && value.starts_with('-'))
                {
                    if (option.value.size() >= min_tokens)
                        break;
                    original_token = value;
                    if (aliases.findOption(value, false))
                        throw po::invalid_command_line_syntax(po::invalid_syntax::missing_parameter);
                }
                option.value.push_back(value);
                option.original_tokens.push_back(value);
                ++argument_index;
            }

            if (option.value.size() < min_tokens)
                throw po::invalid_command_line_syntax(po::invalid_syntax::missing_parameter);
        }
        else
            option.unregistered = true;

        result.options.options.push_back(std::move(option));
        result.descriptors.push_back(descriptor);
    };

    while (argument_index < arguments.size())
    {
        const auto & argument = arguments[argument_index++];
        if (argument == "--")
        {
            while (argument_index < arguments.size())
                append_positional(arguments[argument_index++]);
            break;
        }
        if (argument.size() < 2 || !argument.starts_with('-'))
        {
            append_positional(argument);
            continue;
        }

        original_token = argument;
        try
        {
            if (argument.starts_with("--"))
            {
                const auto equals = argument.find('=');
                option_name = argument.substr(2, equals == std::string::npos ? equals : equals - 2);
                const auto alias = aliases(argument);
                if (!alias.first.empty())
                    option_name = alias.first;
                else if (equals != std::string::npos && equals + 1 == argument.size())
                    throw po::invalid_command_line_syntax(po::invalid_syntax::empty_adjacent_parameter);

                po::option option;
                option.string_key = option_name;
                option.original_tokens.push_back(argument);
                if (equals != std::string::npos && equals + 1 < argument.size())
                    option.value.push_back(argument.substr(equals + 1));
                if (option_name.empty())
                {
                    option.position_key = position_index++;
                    result.options.options.push_back(std::move(option));
                    result.descriptors.push_back(nullptr);
                    continue;
                }
                const auto * descriptor = aliases.findOption(option_name);
                append_option(std::move(option), descriptor);
            }
            else
            {
                for (size_t short_index = 1; short_index < argument.size(); ++short_index)
                {
                    option_name = std::string("-") + argument[short_index];
                    const auto * descriptor = aliases.findOption(option_name, false);
                    po::option option;
                    option.string_key = option_name;
                    const bool grouped = descriptor && (*descriptor)->semantic()->max_tokens() == 0 && short_index + 1 < argument.size();
                    if (!grouped)
                    {
                        option.original_tokens.push_back(argument);
                        if (short_index + 1 < argument.size())
                            option.value.push_back(argument.substr(short_index + 1));
                    }
                    append_option(std::move(option), descriptor);
                    if (!grouped)
                        break;
                }
            }
        }
        catch (po::error_with_option_name & exception)
        {
            exception.add_context(option_name, original_token, result.options.m_options_prefix);
            throw;
        }
    }

    return result;
}

void storeOptions(const ParsedOptions & parsed_options, po::variables_map & options)
{
    const auto & [parsed, descriptors] = parsed_options;
    std::unordered_set<std::string_view> stored;
    stored.reserve(parsed.options.size());

    for (size_t option_index = 0; option_index < parsed.options.size(); ++option_index)
    {
        const auto & option = parsed.options[option_index];
        const auto * descriptor = descriptors[option_index];
        if (!descriptor)
            continue;

        if (stored.insert(option.string_key).second)
        {
            po::options_description single_description;
            single_description.add(*descriptor);
            po::parsed_options single_option(&single_description, parsed.m_options_prefix);
            single_option.options.push_back(option);
            po::store(single_option, options);
        }
        else
        {
            try
            {
                (*descriptor)->semantic()->parse(options.find(option.string_key)->second.value(), option.value, false);
            }
            catch (po::error_with_option_name & exception)
            {
                exception.add_context(
                    option.string_key,
                    option.original_tokens.empty() ? std::string{} : option.original_tokens.front(),
                    parsed.m_options_prefix);
                throw;
            }
        }
    }

    po::store(po::parsed_options(parsed.description, parsed.m_options_prefix), options);
}

}

void ClientApplicationBase::parseAndCheckOptions(OptionsDescription & options_description, po::variables_map & options, Arguments & arguments)
{
    /// boost::program_options rejects an empty value written adjacent to '=' (e.g. `--opt=`)
    /// with "the argument for option should follow immediately after the equal sign". Rewrite
    /// such a token for a known option into the equivalent space-separated form (`--opt` and an
    /// empty value), which boost accepts, so that `--opt=` behaves like `--opt ""` and `set opt=''`.
    {
        /// Fast path: only build the option-name set and rewrite `arguments` when some token
        /// actually looks like `--opt=` (an empty value written adjacent to '='). This keeps
        /// the common startup path cheap, which matters because this file is compiled separately
        /// specifically to avoid slow option parsing affecting `.sh` test timeouts.
        const bool has_empty_adjacent_value = std::any_of(
            arguments.begin(), arguments.end(),
            [](const auto & argument)
            { return argument.starts_with("--") && argument.size() > 3 && argument.back() == '='; });

        if (has_empty_adjacent_value)
        {
            /// Only options that take a value may accept `--opt=` as an empty value; zero-token
            /// switches (e.g. `--no-system-tables`) must keep rejecting `--switch=`.
            std::unordered_set<std::string> value_option_names;
            for (const auto & option : options_description.main_description.value().options())
                if (option->semantic() && option->semantic()->max_tokens() > 0)
                    value_option_names.insert(option->long_name());

            Arguments rewritten;
            rewritten.reserve(arguments.size());
            for (const auto & argument : arguments)
            {
                const auto pos_eq = argument.find('=');
                if (argument.starts_with("--") && pos_eq != std::string::npos && pos_eq + 1 == argument.size())
                {
                    std::string key = argument.substr(2, pos_eq - 2);
                    std::string normalized_key = key;
                    std::replace(normalized_key.begin(), normalized_key.end(), '-', '_');
                    if (!key.empty() && (value_option_names.contains(key) || value_option_names.contains(normalized_key)))
                    {
                        rewritten.push_back(argument.substr(0, pos_eq));
                        rewritten.emplace_back();
                        continue;
                    }
                }
                rewritten.push_back(argument);
            }
            arguments = std::move(rewritten);
        }
    }

    /// Parse main commandline options.
    const auto & description = options_description.main_description.value();
    const OptionsAliasParser aliases(description);
    const auto parsed_options = parseOptions(arguments, description, aliases);
    const auto & parsed = parsed_options.options;

    /// Check unrecognized options without positional options.
    auto unrecognized_options = po::collect_unrecognized(parsed.options, po::collect_unrecognized_mode::exclude_positional);
    if (!unrecognized_options.empty())
    {
        auto hints = this->getHints(unrecognized_options[0]);
        if (!hints.empty())
            throw Exception(ErrorCodes::UNRECOGNIZED_ARGUMENTS, "Unrecognized option '{}'. Maybe you meant {}",
                            unrecognized_options[0], toString(hints));

        throw Exception(ErrorCodes::UNRECOGNIZED_ARGUMENTS, "Unrecognized option '{}'", unrecognized_options[0]);
    }

    /// Check positional options.
    for (const auto & op : parsed.options)
    {
        /// Skip all options after empty `--`. These are processed separately into the Application configuration.
        if (op.string_key.empty() && op.original_tokens[0].starts_with("--"))
            break;

        if (!op.unregistered && op.string_key.empty() && !op.original_tokens[0].starts_with("--")
            && !op.original_tokens[0].empty() && !op.value.empty())
        {
            /// Two special cases for better usability:
            /// - if the option contains a whitespace, it might be a query: clickhouse "SELECT 1"
            /// - if the option is a filesystem file, then it's likely a queries file (clickhouse repro.sql)
            /// These are relevant for interactive usage - user-friendly, but questionable in general.
            /// In case of ambiguity or for scripts, prefer using proper options.

            const auto & token = op.original_tokens[0];
            po::variable_value value(boost::any(op.value), false);

            const char * option = nullptr;
            std::error_code ec;
            if (token.contains(' '))
                option = "query";
            else if (std::filesystem::is_regular_file(std::filesystem::path{token}, ec))
                option = "queries-file";
            else if (token.contains('/') || token.contains('.'))
                /// The argument looks like a file path (contains `/` or `.`) but doesn't exist on disk.
                /// Give a clear "no such file" error rather than the generic "positional option is not supported"
                /// which is confusing when the user meant to pass a file, e.g.:
                ///     $ clickhouse local /tmp/aaa.rep
                ///     Positional option `/tmp/aaa.rep` is not supported.
                throw Exception(ErrorCodes::BAD_ARGUMENTS, "No such file: {}", token);
            else
                throw Exception(ErrorCodes::BAD_ARGUMENTS, "Positional option `{}` is not supported.", token);

            if (!options.emplace(option, value).second)
                throw Exception(ErrorCodes::BAD_ARGUMENTS, "Positional option `{}` is not supported.", token);
        }
    }

    storeOptions(parsed_options, options);
}

OptionsAliasParser::OptionsAliasParser(const boost::program_options::options_description & options)
{
    options_names.reserve(options.options().size());
    long_option_names.reserve(options.options().size());
    for (const auto & option : options.options())
    {
        const auto add_name = [&](const std::string & name, bool is_primary)
        {
            if (name.empty())
                return;

            auto [iterator, inserted] = options_names.emplace(name, IndexedOption{&option, is_primary});
            if (!inserted)
            {
                iterator->second.is_primary |= is_primary;
                if (iterator->second.option != &option)
                    iterator->second.option = nullptr;
            }
        };

        const auto names = option->long_names();
        for (size_t name_index = 0; name_index < names.second; ++name_index)
        {
            add_name(names.first[name_index], names.first[name_index] == option->long_name());
            if (!names.first[name_index].empty())
                long_option_names.emplace_back(names.first[name_index], &option);
        }

        const auto short_name = option->canonical_display_name(po::command_line_style::allow_dash_for_short);
        if (short_name.size() == 2 && short_name[0] == '-')
            add_name(short_name, false);
    }
}

const OptionsAliasParser::Option * OptionsAliasParser::findOption(std::string_view name, bool allow_prefix) const
{
    const auto iterator = options_names.find(name);
    if (iterator != options_names.end())
    {
        if (!iterator->second.option)
            throw po::ambiguous_option({std::string(name), std::string(name)});
        return iterator->second.option;
    }
    if (!allow_prefix)
        return nullptr;

    if (!long_option_names_sorted)
    {
        std::ranges::sort(long_option_names, {}, &NamedOption::first);
        long_option_names_sorted = true;
    }

    std::unordered_set<const Option *> matches;
    std::vector<std::string> alternatives;
    const Option * matched = nullptr;
    for (auto candidate = std::ranges::lower_bound(long_option_names, name, {}, &NamedOption::first);
         candidate != long_option_names.end() && candidate->first.starts_with(name);
         ++candidate)
    {
        if (matches.insert(candidate->second).second)
        {
            matched = candidate->second;
            alternatives.push_back((*matched)->key(std::string(name)));
        }
    }
    if (alternatives.size() > 1)
        throw po::ambiguous_option(alternatives);
    return matched;
}

std::pair<std::string, std::string> OptionsAliasParser::operator()(const std::string & token) const
{
    if (!token.starts_with("--"))
        return {};
    std::string arg = token.substr(2);

    // divide token by '=' to separate key and value if options style=long_allow_adjacent
    auto pos_eq = arg.find('=');
    std::string key = arg.substr(0, pos_eq);

    if (auto iterator = options_names.find(key); iterator != options_names.end() && iterator->second.is_primary)
        // option does not require any changes, because it is already correct
        return {};

    std::replace(key.begin(), key.end(), '-', '_');
    if (auto iterator = options_names.find(key); iterator == options_names.end() || !iterator->second.is_primary)
        // after replacing '-' with '_' argument is still unknown
        return {};

    std::string value;
    if (pos_eq != std::string::npos && pos_eq < arg.size())
        value = arg.substr(pos_eq + 1);

    return {key, value};
}

}
