/*
    Copyright (C) 2009, 2010  Red Hat, Inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
#if HAVE_LOCALE_H
# include <locale.h>
#endif
#include <getopt.h>
#include <syslog.h>
#include "internal_libreport.h"
#include "cli-report.h"

static char *steal_directory_if_needed(char *dump_dir_name)
{
    struct dump_dir *dd = open_directory_for_writing(dump_dir_name,
                                                     /* ask callback */ NULL);

    if (dd)
    {
        dump_dir_name = xstrdup(dd->dd_dirname);
        dd_close(dd);
    }

    return dump_dir_name;
}

int main(int argc, char** argv)
{
    abrt_init(argv);

    setlocale(LC_ALL, "");
    /* Hack:
     * Right-to-left scripts don't work properly in many terminals.
     * Hebrew speaking people say he_IL.utf8 looks so mangled
     * they prefer en_US.utf8 instead.
     */
    const char *msg_locale = setlocale(LC_MESSAGES, NULL);
    if (msg_locale && strcmp(msg_locale, "he_IL.utf8") == 0)
        setlocale(LC_MESSAGES, "en_US.utf8");
#if ENABLE_NLS
    bindtextdomain(PACKAGE, LOCALEDIR);
    textdomain(PACKAGE);
#endif

    GList *event_list = NULL;
    const char *pfx = "";

    /* Can't keep these strings/structs static: _() doesn't support that */
    const char *program_usage_string = _(
        "& [-vsp] -L[PREFIX] [PROBLEM_DIR]\n"
        "   or: & [-vsp] -e EVENT PROBLEM_DIR\n"
        "   or: & [-vsp] -a[y] PROBLEM_DIR\n"
        "   or: & [-vsp] -c[y] PROBLEM_DIR\n"
        "   or: & [-vsp] -r[y|d] PROBLEM_DIR"
    );
    enum {
        OPT_list_events  = 1 << 0,
        OPT_run_event    = 1 << 1,
        OPT_analyze      = 1 << 2,
        OPT_collect      = 1 << 3,
        OPT_report       = 1 << 4,
        OPT_version      = 1 << 5,
        OPT_delete       = 1 << 6,
        OPTMASK_op       = OPT_list_events|OPT_run_event|OPT_analyze|OPT_collect|OPT_report|OPT_version,
        OPTMASK_need_arg = OPT_run_event|OPT_analyze|OPT_collect|OPT_report,
        OPT_y            = 1 << 7,
        OPT_v            = 1 << 8,
        OPT_s            = 1 << 9,
        OPT_p            = 1 << 10,
    };
    /* Keep enum above and order of options below in sync! */
    struct options program_options[] = {
        /*      short_name long_name  value    parameter_name  help */
        OPT_OPTSTRING('L', NULL     , &pfx, "PREFIX",          _("List possible events [which start with PREFIX]")),
        OPT_LIST(     'e', "event"  , &event_list, "EVENT",    _("Run only these events")),
        OPT_BOOL(     'a', "analyze", NULL,                    _("Run analyze event(s) on PROBLEM_DIR")),
        OPT_BOOL(     'c', "collect", NULL,                    _("Run collect event(s) on PROBLEM_DIR")),
        OPT_BOOL(     'r', "report" , NULL,                    _("Analyze, collect and report problem data in PROBLEM_DIR")),
        OPT_BOOL(     'V', "version", NULL,                    _("Display version and exit")),
        OPT_BOOL(     'd', "delete" , NULL,                    _("Remove PROBLEM_DIR after reporting")),
        OPT_BOOL(     'y', "always" , NULL,                    _("Noninteractive: don't ask questions, assume 'yes'")),
        OPT__VERBOSE(&g_verbose),
        OPT_BOOL(     's', NULL     , NULL,                    _("Log to syslog")),
        OPT_BOOL(     'p', NULL     , NULL,                    _("Add program names to log")),
        OPT_END()
    };
    unsigned opts = parse_opts(argc, argv, program_options, program_usage_string);
    unsigned op = (opts & OPTMASK_op);
    if (!op || ((op-1) & op))
        /* "You must specify exactly one operation" */
        show_usage_and_die(program_usage_string, program_options);
    argv += optind;
    argc -= optind;
    if (argc > 1
        /* dont_need_arg == have_arg? bad in both cases:
         * TRUE == TRUE (dont need arg but have) or
         * FALSE == FALSE (need arg but havent).
         * OPT_list_events is an exception, it can be used in both cases.
         */
     || ((op != OPT_list_events) && (!(opts & OPTMASK_need_arg) == argc))
    ) {
        show_usage_and_die(program_usage_string, program_options);
    }

    if (op == OPT_version)
    {
        printf("%s "VERSION"\n", g_progname);
        return 0;
    }

    export_abrt_envvars(opts & OPT_p);
    if (opts & OPT_s)
    {
        openlog(msg_prefix, 0, LOG_DAEMON);
        logmode = LOGMODE_SYSLOG;
    }

    char *dump_dir_name = argv[0];
    bool always = (opts & OPT_y);

    /* Get settings */
    load_event_config_data();

    /* Do the selected operation. */
    int exitcode = 0;
    switch (op)
    {
        case OPT_list_events: /* -L[PREFIX] */
        {
            /* Note that dump_dir_name may be NULL here, it means "show all
             * possible events regardless of dir"
             */
            char *events = list_possible_events(NULL, dump_dir_name, pfx);
            if (!events)
                return 1; /* error msg is already logged */
            fputs(events, stdout);
            free(events);
            break;
        }
        case OPT_run_event: /* -e EVENT: run event */
        {
            dump_dir_name = steal_directory_if_needed(dump_dir_name);
            exitcode = run_events_chain(dump_dir_name, event_list);
            break;
        }
        case OPT_analyze:
        {
            /* Load problem_data from dump dir */
            struct dump_dir *dd = dd_opendir(dump_dir_name, DD_OPEN_READONLY);
            if (!dd)
                return 1;
            char *analyze_events_as_lines = list_possible_events(dd, NULL, "analyze");
            dd_close(dd);

            if (analyze_events_as_lines && *analyze_events_as_lines)
            {
                GList *list_analyze_events = str_to_glist(analyze_events_as_lines, '\n');
                char *event = select_event_option(list_analyze_events);
                list_free_with_free(list_analyze_events);
                exitcode = run_analyze_event(dump_dir_name, event);
                free(event);
            }
            free(analyze_events_as_lines);
            break;
        }
        case OPT_collect:
        {
            exitcode = collect(dump_dir_name, always);

            /* Be consistent and return 1 when opening dd failed */
            if (exitcode == -1)
                return 1;

            break;
        }
        case OPT_report:
        {
            dump_dir_name = steal_directory_if_needed(dump_dir_name);

            exitcode = report(dump_dir_name,
                    (always ? CLI_REPORT_BATCH : 0));

            if (exitcode == -1)
                error_msg_and_die("Crash '%s' not found", dump_dir_name);

            if (opts & OPT_delete)
            {
                int r = delete_dump_dir_possibly_using_abrtd(dump_dir_name);
                if (exitcode == 0)
                    exitcode = r;
            }

            break;
        }
    }

    return exitcode;
}