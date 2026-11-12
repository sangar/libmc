#include "test.h"

#include "mc/encoding/mail.h"
#include "mc/text/utf8.h"

static ParsedMail parse_mail(Test *test, const char *raw)
{
    return mail_parse(test->arena, S(raw));
}

static String body_of(ParsedMail message, size_t index)
{
    return index < message.part_count ? message.parts[index].body : S("(no such part)");
}

static void mail_reads_header_fields(Test *test)
{
    const char *const lf = "From MAILER-DAEMON Mon Jan  1 10:00:00 2024\n"
                           "Received: from a\n\tby b\n"
                           "SUBJECT : Hei p\xC3\xA5 deg\n"
                           "X-Empty:\n"
                           "\n"
                           "Body: not a field\n";
    const String raws[] = { S(lf), str_replace_all(test->arena, S(lf), S("\n"), S("\r\n")) };
    for (size_t i = 0; i < countof(raws); i++) {
        ParsedMail message = mail_parse(test->arena, raws[i]);
        test_check(test, message.header_count == 3, "three fields, the mbox From line skipped");
        if (message.header_count != 3) {
            continue;
        }
        test_check_str(test, message.headers[0].value, S("from a\tby b"), "a folded field is unfolded");
        test_check_str(test, message.headers[1].name, S("SUBJECT"), "a name keeps its case");
        test_check_str(test, message.subject, S("Hei på deg"), "names match ignoring case and space before the colon");
        test_check_str(test, message.headers[2].value, S(""), "an empty field");
        test_check_str(test, str_trim(body_of(message, message.text)), S("Body: not a field"),
                       "the body follows the empty line");
    }

    ParsedMail only_headers = parse_mail(test, "Subject: Bare \xC3\x85se\r\nTo: a@example.no");
    test_check_str(test, only_headers.subject, S("Bare Åse"), "a message with no empty line is all fields");
    test_check(test, only_headers.part_count == 1 && only_headers.text == 0 && body_of(only_headers, 0).len == 0,
               "and an empty text body");

    ParsedMail no_break = parse_mail(test, "Subject: x\nThis line is no field\nnor this");
    test_check_str(test, body_of(no_break, no_break.text), S("This line is no field\nnor this"),
                   "a line that is not a field starts the body");

    ParsedMail empty = parse_mail(test, "");
    test_check(test, empty.header_count == 0 && empty.subject.len == 0 && empty.from.count == 0 && empty.date == 0 &&
                         empty.part_count == 1,
               "an empty message");
}

static void mail_decodes_encoded_words(Test *test)
{
    static const struct {
        const char *value;
        const char *decoded;
    } CASES[] = {
        { "=?UTF-8?Q?Hei_p=C3=A5_deg?=", "Hei på deg" },
        { "=?ISO-8859-1?B?5vjlIMbYxQ==?=", "æøå ÆØÅ" },
        { "=?utf-8?b?w6bDuMOl?= =?utf-8?b?IMOGw5jDhQ==?=", "æøå ÆØÅ" },
        { "=?UTF-8?B?w6U?=\r\n\t=?UTF-8?Q?=C3=A6?=", "åæ" },
        { "=?UTF-8?Q?=C3?= =?UTF-8?Q?=A5?=", "å" },
        { "Re: =?windows-1252?Q?=93Bl=E5b=E6r=94_=80?=", "Re: “Blåbær” €" },
        { "=?iso-8859-15?q?=A4=A6=A8=B4=B8=BC=BD=BE_=C6=D8=C5?=", "€ŠšŽžŒœŸ ÆØÅ" },
        { "=?latin1?Q?=E6?= =?utf-8?Q?=C3=B8?=", "æø" },
        { "=?UTF-8*no?Q?=C3=B8l?=", "øl" },
        { "a =?utf-8?q?=C3=A6?= b", "a æ b" },
        { "=?utf-8?x?abc?=", "=?utf-8?x?abc?=" },
        { "=?utf-8?q?no end", "=?utf-8?q?no end" },
        { "=?utf-8?q?a b?=", "=?utf-8?q?a b?=" },
        { "=??q?a?=", "=??q?a?=" },
        { "100% =?", "100% =?" },
        { "=?x-unknown?Q?=E5?=", "\xEF\xBF\xBD" },
        { "Bl\xE5" "b\xE6r", "Blåbær" },
        { "Bla\xCC\x8A" "b\xC3\xA6r", "Blåbær" },
    };
    for (size_t i = 0; i < countof(CASES); i++) {
        test_check_str(test, mail_decode_words(test->arena, S(CASES[i].value)), S(CASES[i].decoded), CASES[i].value);
    }
}

static void check_address(Test *test, MailAddressList list, size_t index, const char *name, const char *address)
{
    if (index >= list.count) {
        test_check(test, false, address);
        return;
    }
    test_check_str(test, list.items[index].name, S(name), address);
    test_check_str(test, list.items[index].address, S(address), address);
}

static void mail_reads_address_lists(Test *test)
{
    ParsedMail message = parse_mail(
        test, "From: \"Nordmann, Ola \\\"Olsen\\\"\" <ola@example.no>\r\n"
              "To: Kari <kari@example.no>, per@example.no (Per \xC3\x85s),\r\n"
              " =?UTF-8?Q?=C3=85se_=C3=98deg=C3=A5rd?= <ase@example.no>,\r\n"
              " \"=?utf-8?q?Bj=C3=B8rn?=\" <bjorn@example.no>,\r\n"
              " =?utf-8?q?Nordmann,_=C3=98yvind?= <oyvind@example.no>\r\n"
              "Cc: undisclosed-recipients:;\r\n"
              "Bcc: Team: a@example.no, B (bee) <b@example.no>;, c@example.no\r\n"
              "Reply-To: <  svar@example.no  >\r\n"
              "\r\n");
    test_check(test, message.from.count == 1, "one sender");
    check_address(test, message.from, 0, "Nordmann, Ola \"Olsen\"", "ola@example.no");
    test_check(test, message.to.count == 5, "five recipients");
    check_address(test, message.to, 0, "Kari", "kari@example.no");
    check_address(test, message.to, 1, "Per Ås", "per@example.no");
    check_address(test, message.to, 2, "Åse Ødegård", "ase@example.no");
    check_address(test, message.to, 3, "Bjørn", "bjorn@example.no");
    check_address(test, message.to, 4, "Nordmann, Øyvind", "oyvind@example.no");
    test_check(test, message.cc.count == 0, "an empty group");
    test_check(test, message.bcc.count == 3, "a group's mailboxes and one after it");
    check_address(test, message.bcc, 0, "", "a@example.no");
    check_address(test, message.bcc, 1, "B", "b@example.no");
    check_address(test, message.bcc, 2, "", "c@example.no");
    check_address(test, message.reply_to, 0, "", "svar@example.no");
}

static int64_t date_of(Test *test, String date)
{
    String raw = str_format(test->arena, "Date: %.*s\r\n\r\n", (int)date.len, date.data);
    return mail_parse(test->arena, raw).date;
}

static void mail_reads_dates(Test *test)
{
    const int64_t nine = 1704099600 * (int64_t)NS_PER_SECOND; // 2024-01-01 09:00:00 UTC
    static const char *const SAME[] = {
        "Mon, 01 Jan 2024 10:00:00 +0100",
        "1 Jan 2024 10:00:00 +0100",
        "Mon, 1 Jan 2024 10:00 +0100",
        "Mon,1 Jan 2024 10:00:00 +0100 (CET)",
        "Mon, 1 Jan 24 10:00:00 +0100",
        "Mon, 1 Jan 2024 08:30:00 -0030",
        "Mon, 1 Jan 2024 09:00:00 UT",
        "Mon, 1 Jan 2024 09:00:00 GMT",
        "Mon, 1 Jan 2024 09:00:00 Z",
        "Mon, 1 Jan 2024 09:00:00",
    };
    for (size_t i = 0; i < countof(SAME); i++) {
        test_check(test, date_of(test, S(SAME[i])) == nine, SAME[i]);
    }
    static const struct {
        const char *zone;
        int hour;
    } ZONES[] = {
        { "EST", 4 }, { "EDT", 5 }, { "CST", 3 }, { "CDT", 4 }, { "MST", 2 }, { "MDT", 3 }, { "PST", 1 }, { "pdt", 2 },
    };
    for (size_t i = 0; i < countof(ZONES); i++) {
        String date = str_format(test->arena, "Mon, 1 Jan 2024 %02d:00:00 %s", ZONES[i].hour, ZONES[i].zone);
        test_check(test, date_of(test, date) == nine, ZONES[i].zone);
    }
    test_check(test, date_of(test, S("Fri, 31 Dec 99 23:59:59 -0000")) == 946684799 * (int64_t)NS_PER_SECOND,
               "a two-digit year before 50 is in the 1900s");
    static const char *const MALFORMED[] = {
        "",
        "yesterday",
        "Fri, 30 Feb 2024 10:00:00 +0000",
        "0 Jan 2024 10:00:00 +0000",
        "32 Jan 2024 10:00:00 +0000",
        "1 Foo 2024 10:00:00 +0000",
        "1 Jan 2024 24:00:00 +0000",
        "1 Jan 2024",
        "1 Jan 5 10:00:00 +0000",
        "1 Jan 99999 10:00:00 +0000",
        "2024-01-01T10:00:00Z",
    };
    for (size_t i = 0; i < countof(MALFORMED); i++) {
        test_check(test, date_of(test, S(MALFORMED[i])) == 0, MALFORMED[i]);
    }
}

static void mail_formats_dates(Test *test)
{
    const int64_t nine = 1704099600 * (int64_t)NS_PER_SECOND; // 2024-01-01 09:00:00 UTC, a Monday
    test_check(test, str_equal(mail_format_date(test->arena, nine), S("Mon, 01 Jan 2024 09:00:00 +0000")), "a Monday");
    test_check(test, str_equal(mail_format_date(test->arena, 0), S("Thu, 01 Jan 1970 00:00:00 +0000")), "the epoch");
    int64_t leap = 1709251199 * (int64_t)NS_PER_SECOND + 999999999; // the last nanosecond of 29 Feb 2024
    test_check(test, str_equal(mail_format_date(test->arena, leap), S("Thu, 29 Feb 2024 23:59:59 +0000")),
               "a leap day, the nanoseconds dropped");
    test_check(test, date_of(test, mail_format_date(test->arena, leap)) == leap - 999999999, "mail_parse reads it back");
}

static void mail_reads_message_ids(Test *test)
{
    ParsedMail message = parse_mail(test, "Message-ID:  <abc@example.no> \r\n"
                                          "In-Reply-To: Your message of Monday <first@example.no>\r\n"
                                          " <second@example.no>\r\n"
                                          "References: <one@example.no>\r\n <two@example.no>\r\n"
                                          "\t<three@\r\n example.no>\r\n"
                                          "\r\n");
    test_check_str(test, message.message_id, S("abc@example.no"), "Message-ID");
    test_check_str(test, message.in_reply_to, S("first@example.no"), "the first id of In-Reply-To");
    test_check(test, message.references.count == 3, "three references");
    if (message.references.count == 3) {
        test_check_str(test, message.references.items[0], S("one@example.no"), "oldest first");
        test_check_str(test, message.references.items[2], S("three@example.no"), "folded inside an id");
    }
    ParsedMail bare = parse_mail(test, "Message-ID: bare@example.no\r\n\r\n");
    test_check_str(test, bare.message_id, S("bare@example.no"), "an id without angle brackets");
    test_check(test, parse_mail(test, "Subject: x\r\n\r\n").message_id.len == 0, "no Message-ID");
}

static void mail_reads_parameters_and_file_names(Test *test)
{
    ParsedMail message = parse_mail(
        test, "Content-Type: multipart/mixed; boundary=\"=_b quoted\"\r\n"
              "\r\n"
              "--=_b quoted\r\n"
              "Content-Type: TEXT/Plain; charset=\"UTF-8\"\r\n"
              "\r\n"
              "Hei\r\n"
              "--=_b quoted\r\n"
              "Content-Type: application/pdf\r\n"
              "Content-Disposition: attachment; filename*=UTF-8''bl%C3%A5b%C3%A6r.pdf\r\n"
              "\r\n"
              "%PDF\r\n"
              "--=_b quoted\r\n"
              "Content-Type: text/plain; charset=us-ascii\r\n"
              "Content-Disposition: attachment;\r\n"
              " filename*0*=iso-8859-1''%C6%D8%C5;\r\n"
              " filename*1=\" og mer.txt\"\r\n"
              "\r\n"
              "ÆØÅ\r\n"
              "--=_b quoted\r\n"
              "Content-Type: image/jpeg; name=\"=?UTF-8?Q?F=C3=B8dsel.jpg?=\"\r\n"
              "\r\n"
              "jpeg\r\n"
              "--=_b quoted\r\n"
              "Content-Type: image/png\r\n"
              "Content-Disposition: inline; filename=\"logo \\\"stor\\\".png\"\r\n"
              "Content-ID: <logo@example.no>\r\n"
              "\r\n"
              "png\r\n"
              "--=_b quoted\r\n"
              "Content-Type: application/octet-stream\r\n"
              "Content-Disposition: attachment; filename=\"bla\xCC\x8A.bin\"\r\n"
              "\r\n"
              "--=_b quoted--\r\n");
    test_check(test, message.part_count == 6, "six parts");
    if (message.part_count != 6) {
        return;
    }
    const MailPart *parts = message.parts;
    test_check_str(test, parts[0].content_type, S("text/plain"), "the type in lower case");
    test_check_str(test, parts[0].charset, S("utf-8"), "the charset in lower case");
    test_check(test, !parts[0].attachment && parts[0].filename.len == 0, "the body is no attachment");
    test_check_str(test, parts[1].filename, S("blåbær.pdf"), "an RFC 2231 file name");
    test_check(test, parts[1].attachment, "Content-Disposition: attachment");
    test_check_str(test, parts[2].filename, S("ÆØÅ og mer.txt"), "RFC 2231 sections in ISO-8859-1");
    test_check(test, parts[2].attachment && message.text == 0, "a text attachment is not the body");
    test_check_str(test, parts[3].filename, S("Fødsel.jpg"), "an RFC 2047 name on Content-Type");
    test_check(test, parts[3].attachment, "a part that is not text and has no disposition");
    test_check_str(test, parts[4].filename, S("logo \"stor\".png"), "a quoted file name");
    test_check_str(test, parts[4].content_id, S("logo@example.no"), "Content-ID");
    test_check(test, !parts[4].attachment, "an inline image with a file name");
    test_check_str(test, parts[5].filename, S("blå.bin"), "a decomposed file name in NFC");
    test_check(test, parts[5].body.len == 0, "an empty part");
}

static void mail_walks_multipart_trees(Test *test)
{
    static const char RAW[] = "Content-Type: multipart/mixed; boundary=\"outer\"\n"
                              "\n"
                              "This is the preamble.\n"
                              "--outer\n"
                              "Content-Type: multipart/alternative; boundary=inner\n"
                              "\n"
                              "--inner\n"
                              "Content-Type: text/plain; charset=utf-8\n"
                              "\n"
                              "Plain æøå\n"
                              "--inner \t\n"
                              "Content-Type: multipart/related; boundary=\"rel\"\n"
                              "\n"
                              "--rel\n"
                              "Content-Type: text/html; charset=utf-8\n"
                              "\n"
                              "<p>HTML ÆØÅ</p>\n"
                              "--rel\n"
                              "Content-Type: image/png\n"
                              "Content-ID: <img@x>\n"
                              "Content-Transfer-Encoding: base64\n"
                              "\n"
                              "iVBORw==\n"
                              "--rel--\n"
                              "The epilogue.\n"
                              "--inner--\n"
                              "--outer\n"
                              "Content-Type: message/rfc822\n"
                              "\n"
                              "Subject: Forwarded\n"
                              "Content-Type: text/plain\n"
                              "\n"
                              "Inner body\n"
                              "--outer\n"
                              "Content-Type: text/plain\n"
                              "Content-Disposition: attachment; filename=notes.txt\n"
                              "\n"
                              "Notes\n";
    ParsedMail message = parse_mail(test, RAW);
    test_check(test, message.part_count == 5, "five leaves");
    if (message.part_count != 5) {
        return;
    }
    test_check(test, message.text == 0 && message.html == 1, "the text and HTML bodies");
    test_check_str(test, body_of(message, 0), S("Plain æøå"), "the line break before a boundary belongs to it");
    test_check_str(test, body_of(message, 1), S("<p>HTML ÆØÅ</p>"), "a part of a related part");
    test_check_str(test, body_of(message, 2), S("\x89PNG"), "a base64 image");
    test_check(test, message.parts[2].attachment && str_equal(message.parts[2].content_id, S("img@x")),
               "an image with no disposition is an attachment");
    test_check(test, message.parts[3].attachment && str_equal(message.parts[3].content_type, S("message/rfc822")),
               "a forwarded message is one attachment");
    test_check_str(test, body_of(message, 3), S("Subject: Forwarded\nContent-Type: text/plain\n\nInner body"),
                   "with its raw bytes");
    test_check_str(test, body_of(message, 4), S("Notes\n"), "a missing closing boundary ends the last part at the end");

    ParsedMail unsplit = parse_mail(test, "Content-Type: multipart/mixed; boundary=zzz\n\nJust text");
    test_check(test, unsplit.part_count == 1 && unsplit.text == 0, "a multipart with no boundary in it is text");
    test_check_str(test, body_of(unsplit, 0), S("Just text"), "its body as it is");

    for (size_t len = 0; len <= sizeof RAW - 1; len++) {
        ParsedMail cut = mail_parse(test->arena, (String){ RAW, len });
        bool valid = true;
        for (size_t i = 0; i < cut.part_count; i++) {
            bool text = str_starts_with(cut.parts[i].content_type, S("text/"));
            valid = valid && (!text || utf8_is_valid(cut.parts[i].body));
        }
        if (!valid) {
            test_check(test, false, "every text body of a message cut short is valid UTF-8");
            break;
        }
    }
}

static String single_part(Test *test, const char *encoding, const char *body)
{
    String raw = str_format(test->arena,
                            "Content-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: %s\r\n\r\n%s",
                            encoding, body);
    return body_of(mail_parse(test->arena, raw), 0);
}

static void mail_decodes_transfer_encodings(Test *test)
{
    test_check_str(test, single_part(test, "base64", "w6bDuMOl\r\nIMOGw5jD\nhQ\r\n"), S("æøå ÆØÅ"),
                   "base64 with line breaks and no padding");
    test_check_str(test, single_part(test, "BASE64", "w6bDuMOl\r\n=\r\nIMOGw5jDhQ==\r\n"), S("æøå ÆØÅ"),
                   "base64 joined after its padding");
    String quoted =
        single_part(test, "quoted-printable", "Bl=C3=A5b=c3=a6r og s=\r\nyltet=C3=B8y =ZZ 100%= \r\n ok = end=\r\n");
    test_check_str(test, quoted, S("Blåbær og syltetøy =ZZ 100% ok = end"),
                   "quoted-printable with soft line breaks and a bad escape");
    test_check_str(test, single_part(test, "8bit", "Blåbær\r\n"), S("Blåbær\r\n"), "8bit as it is");
    test_check_str(test, single_part(test, "x-uuencode", "=C3=A5"), S("=C3=A5"), "an unknown encoding as it is");
}

static void mail_converts_text_to_utf8(Test *test)
{
    static const struct {
        const char *charset;
        const char *body;
        const char *text;
    } CASES[] = {
        { "utf-8", "Bl\xC3\xA5" "b\xFF", "Blåb\xEF\xBF\xBD" },
        { "UTF8", "\xC3\x86\xC3\x98\xC3\x85", "ÆØÅ" },
        { "iso-8859-1", "\xC6\xD8\xC5 \xE6\xF8\xE5", "ÆØÅ æøå" },
        { "LATIN1", "\xC6\xD8\xC5 \xE6\xF8\xE5", "ÆØÅ æøå" },
        { "windows-1252", "\x93" "Bl\xE5" "b\xE6r\x94 \x80 5", "“Blåbær” € 5" },
        { "cp1252", "\x91\xC5\x92 \x96 \xC6\xD8", "‘Å’ – ÆØ" },
        { "iso-8859-15", "\xA4\xA6\xA8\xB4\xB8\xBC\xBD\xBE \xE6\xF8\xE5", "€ŠšŽžŒœŸ æøå" },
        { "us-ascii", "Bl\xE5" "b\xE6r", "Blåbær" },
        { "us-ascii", "Bl\xC3\xA5" "b\xC3\xA6r", "Blåbær" },
        { "", "\xC3\x85se", "Åse" },
        { "x-unknown", "\xE5", "\xEF\xBF\xBD" },
    };
    for (size_t i = 0; i < countof(CASES); i++) {
        String raw = str_format(test->arena, "Content-Type: text/plain; charset=\"%s\"\r\n\r\n%s", CASES[i].charset,
                                CASES[i].body);
        ParsedMail message = mail_parse(test->arena, raw);
        test_check_str(test, body_of(message, message.text), S(CASES[i].text), CASES[i].charset);
    }
    ParsedMail binary = parse_mail(test, "Content-Type: application/octet-stream\r\n\r\n\xFF\xFE");
    test_check_str(test, body_of(binary, 0), S("\xFF\xFE"), "a part that is not text keeps its bytes");
}

static void mail_bounds_hostile_trees(Test *test)
{
    for (size_t levels = 16; levels <= 17; levels++) {
        StringBuilder raw = str_builder_create(test->arena, 4096);
        for (size_t i = 0; i < levels; i++) {
            str_builder_append_format(&raw, "Content-Type: multipart/mixed; boundary=b%zu\n\n--b%zu\n", i, i);
        }
        str_builder_append(&raw, S("Content-Type: text/plain\n\nDeep\n"));
        ParsedMail message = mail_parse(test->arena, str_builder_finish(&raw));
        bool found = message.part_count == 1 && str_equal(body_of(message, 0), S("Deep\n"));
        test_check(test, levels == 16 ? found : message.part_count == 0,
                   levels == 16 ? "16 levels are read" : "17 are not");
    }
    StringBuilder raw = str_builder_create(test->arena, 4096);
    str_builder_append(&raw, S("Content-Type: multipart/mixed; boundary=b\n\n"));
    for (size_t i = 0; i < 600; i++) {
        str_builder_append(&raw, S("--b\n\nx\n"));
    }
    test_check(test, mail_parse(test->arena, str_builder_finish(&raw)).part_count == 512, "at most 512 parts");
}

const TestCase MAIL_TESTS[] = {
    { "mail_reads_header_fields", mail_reads_header_fields },
    { "mail_decodes_encoded_words", mail_decodes_encoded_words },
    { "mail_reads_address_lists", mail_reads_address_lists },
    { "mail_reads_dates", mail_reads_dates },
    { "mail_formats_dates", mail_formats_dates },
    { "mail_reads_message_ids", mail_reads_message_ids },
    { "mail_reads_parameters_and_file_names", mail_reads_parameters_and_file_names },
    { "mail_walks_multipart_trees", mail_walks_multipart_trees },
    { "mail_decodes_transfer_encodings", mail_decodes_transfer_encodings },
    { "mail_converts_text_to_utf8", mail_converts_text_to_utf8 },
    { "mail_bounds_hostile_trees", mail_bounds_hostile_trees },
    { nullptr, nullptr },
};
