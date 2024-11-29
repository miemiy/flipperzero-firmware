#include "cli_ansi.h"

typedef enum {
    CliAnsiParserStateInitial,
    CliAnsiParserStateEscape,
    CliAnsiParserStateEscapeBrace,
    CliAnsiParserStateEscapeBraceOne,
    CliAnsiParserStateEscapeBraceOneSemicolon,
    CliAnsiParserStateEscapeBraceOneSemicolonModifiers,
} CliAnsiParserState;

struct CliAnsiParser {
    CliAnsiParserState state;
    CliModKey modifiers;
};

CliAnsiParser* cli_ansi_parser_alloc(void) {
    CliAnsiParser* parser = malloc(sizeof(CliAnsiParser));
    return parser;
}

void cli_ansi_parser_free(CliAnsiParser* parser) {
    free(parser);
}

/**
 * @brief Converts a single character representing a special key into the enum
 * representation
 */
static CliKey cli_ansi_key_from_mnemonic(char c) {
    switch(c) {
    case 'A':
        return CliKeyUp;
    case 'B':
        return CliKeyDown;
    case 'C':
        return CliKeyRight;
    case 'D':
        return CliKeyLeft;
    case 'F':
        return CliKeyEnd;
    case 'H':
        return CliKeyHome;
    default:
        return CliKeyUnrecognized;
    }
}

CliAnsiParserResult cli_ansi_parser_feed(CliAnsiParser* parser, char c) {
    switch(parser->state) {
    case CliAnsiParserStateInitial:
        // <key> -> <key>
        if(c != CliKeyEsc) {
            return (CliAnsiParserResult){
                .is_done = true,
                .result = (CliKeyCombo){
                    .modifiers = CliModKeyNo,
                    .key = c,
                }};
        }

        // <ESC> ...
        parser->state = CliAnsiParserStateEscape;
        break;

    case CliAnsiParserStateEscape:
        // <ESC> <ESC> -> <ESC>
        if(c == CliKeyEsc) {
            parser->state = CliAnsiParserStateInitial;
            return (CliAnsiParserResult){
                .is_done = true,
                .result = (CliKeyCombo){
                    .modifiers = CliModKeyNo,
                    .key = c,
                }};
        }

        // <ESC> <key> -> Alt + <key>
        if(c != '[') {
            parser->state = CliAnsiParserStateInitial;
            return (CliAnsiParserResult){
                .is_done = true,
                .result = (CliKeyCombo){
                    .modifiers = CliModKeyAlt,
                    .key = c,
                }};
        }

        // <ESC> [ ...
        parser->state = CliAnsiParserStateEscapeBrace;
        break;

    case CliAnsiParserStateEscapeBrace:
        // <ESC> [ <key mnemonic> -> <key>
        if(c != '1') {
            parser->state = CliAnsiParserStateInitial;
            return (CliAnsiParserResult){
                .is_done = true,
                .result = (CliKeyCombo){
                    .modifiers = CliModKeyNo,
                    .key = cli_ansi_key_from_mnemonic(c),
                }};
        }

        // <ESC> [ 1 ...
        parser->state = CliAnsiParserStateEscapeBraceOne;
        break;

    case CliAnsiParserStateEscapeBraceOne:
        // <ESC> [ 1 <non-;> -> error
        if(c != ';') {
            parser->state = CliAnsiParserStateInitial;
            return (CliAnsiParserResult){
                .is_done = true,
                .result = (CliKeyCombo){
                    .key = CliKeyUnrecognized,
                }};
        }

        // <ESC> [ 1 ; ...
        parser->state = CliAnsiParserStateEscapeBraceOneSemicolon;
        break;

    case CliAnsiParserStateEscapeBraceOneSemicolon:
        // <ESC> [ 1 ; <modifiers> ...
        parser->modifiers = (c - '0');
        parser->modifiers &= ~1;
        parser->state = CliAnsiParserStateEscapeBraceOneSemicolonModifiers;
        break;

    case CliAnsiParserStateEscapeBraceOneSemicolonModifiers:
        // <ESC> [ 1 ; <modifiers> <key mnemonic> -> <modifiers> + <key>
        parser->state = CliAnsiParserStateInitial;
        return (CliAnsiParserResult){
            .is_done = true,
            .result = (CliKeyCombo){
                .modifiers = parser->modifiers,
                .key = cli_ansi_key_from_mnemonic(c),
            }};
    }

    return (CliAnsiParserResult){.is_done = false};
}
