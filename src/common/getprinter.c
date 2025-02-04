/***************************************************************************
 * LPRng - An Extended Print Spooler System
 *
 * Copyright 1988-2003, Patrick Powell, San Diego, CA
 *     papowell@lprng.com
 * See LICENSE for conditions of use.
 *
 ***************************************************************************/

#include "lp.h"
#include "gethostinfo.h"
#include "getprinter.h"
#include "getqueue.h"
#include "child.h"
/**** ENDINCLUDE ****/

/***************************************************************************
 Get_printer()
    determine the name of the printer - Printer_DYN variable
	Note: this is used by clients to find the name of default printer
	or by server to find forwarding information.  If the printcap
	RemotePrinter_DYN is specified this overrides the printer name.
	1. -P option
	2. $PRINTER, $LPDEST, $NPRINTER, $NGPRINTER argument variable
	3. printcap file
	4. "lp" if none specified
	5. Get the printcap entry (if any),  and re-extract information-
        - printer name (primary name)
		- lp=printer@remote or rp@rm information
    6. recheck the printer name for printer@hostname form,
       and set RemoteHost_DYN to the hostname
	Note: this appears to cover all the cases, with the exception that
	a primary name of the form printer@host will be detected as the
	destination.  Sigh...
 ***************************************************************************/

char *Get_printer(void)
{
	char *s = Printer_DYN;

	DEBUG1("Get_printer: original printer '%s'", s );
	if( s == 0 ) s = getenv( "PRINTER" );
	if( s == 0 ) s = getenv( "LPDEST" );
	if( s == 0 ) s = getenv( "NPRINTER" );
	if( s == 0 ) s = getenv( "NGPRINTER" );

	if( !Require_explicit_Q_DYN ){

		if( s == 0 ){
			Get_all_printcap_entries();
			if( All_line_list.count ){
				s = All_line_list.list[0];
			}
		}
		if( s == 0 ) s = Default_printer_DYN;
	}
	if( s == 0 ){
		fatal(LOG_ERR, "No printer name available, usage: 'lpr -Pprinter filename'" );
	}
	Set_DYN(&Printer_DYN,s);
	Expand_vars();
	DEBUG1("Get_printer: final printer '%s'",Printer_DYN);
	return(Printer_DYN);
}

/***************************************************************************
 * Fix_Rm_Rp_info
 *  - get the remote host and remote printer information
 *  - we assume this is called by clients trying to get remote host
 *    connection information
 *  - we may want to get the printcap information as a side effect
 *
 ***************************************************************************/


void Fix_Rm_Rp_info(char *report_conflict, int report_len )
{
	char *s;

	DEBUG1("Fix_Rm_Rp_info: printer name '%s'", Printer_DYN );

	/*
	 * now check to see if we have a remote printer
	 * 1. printer@host form overrides
	 * 2. printcap entry, we use lp=pr@host
	 * 3. printcap entry, we use remote host, remote printer
	 * 4. no printcap entry, we use default printer, default remote host
	 */
	s = Printer_DYN;
	Printer_DYN = 0;
	Reset_config();
	Printer_DYN = s;
	Free_line_list(&PC_alias_line_list);
	Free_line_list(&PC_entry_line_list);
	Set_DYN(&Lp_device_DYN, 0 );
	Set_DYN(&RemotePrinter_DYN, 0 );
	Set_DYN(&RemoteHost_DYN, 0 );

	if( !Is_server ){
		if( (s = safestrchr( Printer_DYN, '@' ))  ){
			Set_DYN(&RemotePrinter_DYN, Printer_DYN );
			*s = 0;
			Set_DYN(&Queue_name_DYN, Printer_DYN );
			s = safestrchr( RemotePrinter_DYN, '@');
			*s++ = 0;
			Set_DYN(&RemoteHost_DYN, s );
			if( (s = safestrchr(RemoteHost_DYN,'%')) ){
				Set_DYN(&Unix_socket_path_DYN, 0 );
			}
			/* force connection via TCP/IP */
			goto done;
		}
		/* we search for the values in the printcap */
		Set_DYN(&Queue_name_DYN, Printer_DYN );
		s = 0;
		if(
			(s = Select_pc_info(Printer_DYN,
			&PC_entry_line_list,
			&PC_alias_line_list,
			&PC_names_line_list, &PC_order_line_list,
			&PC_info_line_list, 0, 1 ))
			||
			(s = Select_pc_info("*",
			&PC_entry_line_list,
			&PC_alias_line_list,
			&PC_names_line_list, &PC_order_line_list,
			&PC_info_line_list, 0, 0 ))
		){
			if( !safestrcmp( s, "*" ) ){
				s = Queue_name_DYN;
			}
			Set_DYN(&Printer_DYN,s);

			DEBUG2("Fix_Rm_Rp_info: from printcap found '%s'", Printer_DYN );
			if(DEBUGL2)Dump_line_list("Fix_Rm_Rp_info - PC_alias_line_list",
				&PC_alias_line_list );
			if(DEBUGL2)Dump_line_list("Fix_Rm_Rp_info - PC_entry_line_list",
				&PC_entry_line_list );
		}
		if(DEBUGL2)Dump_line_list("Fix_Rm_Rp_info - final PC_entry_line_list",
			&PC_entry_line_list );
		Find_default_tags( &PC_entry_line_list, Pc_var_list, "client." );
		Find_tags( &PC_entry_line_list, &Config_line_list, "client." );
		Find_tags( &PC_entry_line_list, &PC_entry_line_list, "client." );
		Set_var_list( Pc_var_list, &PC_entry_line_list);
		if( RemoteHost_DYN && Lp_device_DYN && report_conflict ){
			plp_snprintf(report_conflict,report_len,
				"conflicting printcap entries :lp=%s:rm=%s",
				Lp_device_DYN, RemoteHost_DYN );
		}
		/* if a client and have direct, then we need to use
		 * the LP values
		 */
		Expand_percent( &Lp_device_DYN );
		if( Direct_DYN ){
			DEBUG2("Fix_Rm_Rp_info: direct to '%s'", Lp_device_DYN );
			if( strchr( "/|", cval(Lp_device_DYN)) ){
				Set_DYN(&RemotePrinter_DYN, 0 );
				Set_DYN(&RemoteHost_DYN, 0 );
				goto done;
			}
			if( (s = safestrchr( Lp_device_DYN, '@' ))  ){
				Set_DYN(&RemotePrinter_DYN, Lp_device_DYN );
				*s = 0;
				Set_DYN(&Queue_name_DYN, Printer_DYN );
				s = safestrchr( RemotePrinter_DYN, '@');
				*s++ = 0;
				Set_DYN(&RemoteHost_DYN, s );
				if( (s = safestrchr(RemoteHost_DYN,'%')) ){
					Set_DYN(&Unix_socket_path_DYN, 0 );
				}
				goto done;
			}
		}
		if( Force_localhost_DYN ){
			DEBUG2("Fix_Rm_Rp_info: force_localhost to '%s'", Printer_DYN );
			Set_DYN( &RemoteHost_DYN, LOCALHOST );
			Set_DYN( &RemotePrinter_DYN, Printer_DYN );
			Set_DYN( &Lp_device_DYN, 0 );
			goto done;
		}
		if( (s = safestrchr( Lp_device_DYN, '@' ))  ){
			DEBUG2("Fix_Rm_Rp_info: Lp_device_DYN is printer '%s'", Lp_device_DYN );
			Set_DYN(&RemotePrinter_DYN, Lp_device_DYN );
			if( (s = safestrchr( RemotePrinter_DYN,'@')) ){
				*s++ = 0;
				Set_DYN(&RemoteHost_DYN, s );
				if( (s = safestrchr(RemoteHost_DYN+1,'%')) ){
					Set_DYN(&Unix_socket_path_DYN, 0 );
				}
			}
		}
		if( RemoteHost_DYN == 0 || *RemoteHost_DYN == 0 ){
			Set_DYN( &RemoteHost_DYN, Default_remote_host_DYN );
		}
		if( RemoteHost_DYN == 0 || *RemoteHost_DYN == 0 ){
			Set_DYN( &RemoteHost_DYN, FQDNHost_FQDN );
		}
		if( RemotePrinter_DYN == 0 || *RemotePrinter_DYN == 0 ){
			Set_DYN( &RemotePrinter_DYN, Printer_DYN );
		}
		goto done;
	}
	/* we are a server */
	/* we search for the values in the printcap */
	s = 0;
	Set_DYN(&Queue_name_DYN, Printer_DYN );
	if(
		(s = Select_pc_info(Printer_DYN,
		&PC_entry_line_list,
		&PC_alias_line_list,
		&PC_names_line_list, &PC_order_line_list,
		&PC_info_line_list, 0, 1 ))
		||
		(s = Select_pc_info("*",
		&PC_entry_line_list,
		&PC_alias_line_list,
		&PC_names_line_list, &PC_order_line_list,
		&PC_info_line_list, 0, 0 ))
	){
		if( !safestrcmp( s, "*" ) ){
			s = Queue_name_DYN;
		}
		Set_DYN(&Printer_DYN,s);
		DEBUG2("Fix_Rm_Rp_info: found '%s'", Printer_DYN );
	}
	if(DEBUGL2)Dump_line_list("Fix_Rm_Rp_info - PC_alias_line_list",
		&PC_alias_line_list );
	if(DEBUGL2)Dump_line_list("Fix_Rm_Rp_info - PC_entry_line_list",
		&PC_entry_line_list );
	/* now get the Server_xxx variables */
	Find_default_tags( &PC_entry_line_list, Pc_var_list, "server." );
	Find_tags( &PC_entry_line_list, &Config_line_list, "server." );
	Find_tags( &PC_entry_line_list, &PC_entry_line_list, "server." );
	Set_var_list( Pc_var_list, &PC_entry_line_list);
	if( RemoteHost_DYN && Lp_device_DYN && report_conflict ){
		plp_snprintf(report_conflict,report_len,
			"conflicting printcap entries :lp=%s:rm=%s",
			Lp_device_DYN, RemoteHost_DYN );
	}
	if( safestrchr( Lp_device_DYN, '@' ) ){
		Set_DYN(&RemotePrinter_DYN, Lp_device_DYN );
		s = safestrchr( RemotePrinter_DYN, '@');
		if( s ) *s++ = 0;
		else if( *s == 0 ) s = 0;
		Set_DYN(&RemoteHost_DYN, s );
		if( (s = safestrchr(RemoteHost_DYN,'%')) ){
			Set_DYN(&Unix_socket_path_DYN, 0 );
		}
		Set_DYN(&Lp_device_DYN,0);
	} else if( Lp_device_DYN ){
		Set_DYN(&RemoteHost_DYN,0);
		Set_DYN(&RemotePrinter_DYN,0);
	} else if( RemoteHost_DYN ){
		; /* we use defaults */
	} else if( Server_names_DYN == 0 ){
		if( report_conflict ){
			plp_snprintf(report_conflict,report_len,
				"no :rm, :lp, or :sv entry" );
		}
	}
	if( !Lp_device_DYN ){
		if( ISNULL(RemoteHost_DYN) ){
			Set_DYN( &RemoteHost_DYN, Default_remote_host_DYN );
		}
		if( ISNULL(RemoteHost_DYN) ){
			Set_DYN( &RemoteHost_DYN, FQDNHost_FQDN );
		}
		if( ISNULL(RemotePrinter_DYN) ){
			Set_DYN( &RemotePrinter_DYN, Printer_DYN );
		}
	}
 done:

	Expand_vars();
	/*
	 * make sure that these entries are in the printcap file
	 */
	if( Pc_entries_required_DYN){
		struct line_list list;
		struct keywords *var_list = Pc_var_list;
		int i;
		Init_line_list( &list );
		Split(&list,Pc_entries_required_DYN,File_sep,0,0,0,0,0,0);
		for( i = 0; i < list.count; ++i ){
			const char *t;
			int mid;
			char *s = list.list[i];
			if( ISNULL(s) ) continue;
			DEBUG3( "Fix_Rm_Rp_info: checking '%s'", s );
			if( !Find_first_key( &PC_entry_line_list, s, Hash_value_sep, &mid ) ){
				t = safestrpbrk(PC_entry_line_list.list[mid], Option_value_sep );
				DEBUG1( "Fix_Rm_Rp_info: FOUND %s VALUE %s", s, t );
				continue;
			}
			if( !Find_first_key( &Config_line_list, s, Hash_value_sep, &mid ) ){
				t = safestrpbrk(Config_line_list.list[mid], Hash_value_sep );
				DEBUG1( "Fix_Rm_Rp_info: CONFIG %s VALUE %s", s, t );
				Set_str_value(&PC_entry_line_list, s, t);
				continue;
			}
			for( var_list = Pc_var_list; var_list->keyword; ++var_list ){
				if( !strcmp(var_list->keyword, s) ){
					int type = var_list->type;				/* type of entry */
					int v;
					void *p = var_list->variable;	/* address of variable */
					if( !(p) ) break;
					switch(type){
					case FLAG_K:
						v =	*(int *)(p);
						DEBUG1( "Fix_Rm_Rp_info: VAR %s FLAG %d", var_list->keyword, v);
						Set_flag_value(&PC_entry_line_list, var_list->keyword, v);
						break;
					case INTEGER_K:
						v =	*(int *)(p);
						DEBUG1( "Fix_Rm_Rp_info: VAR %s INT %d", var_list->keyword, v);
						Set_decimal_value(&PC_entry_line_list, var_list->keyword, v);
						break;
					case STRING_K:
						t = *(char **)(p);
						DEBUG1( "Fix_Rm_Rp_info: VAR %s= '%s'", var_list->keyword, t );
						if( t ){
							Set_str_value(&PC_entry_line_list, var_list->keyword, t);
						}
						break;
					default:
						break;
					}
					break;
				}
			}
		}
		Free_line_list( &list );
	}
	DEBUG1("Fix_Rm_Rp_info: Printer '%s', Queue '%s', Lp '%s', Rp '%s', Rh '%s'",
		Printer_DYN, Queue_name_DYN, Lp_device_DYN,
		RemotePrinter_DYN, RemoteHost_DYN );
	if(DEBUGL2)Dump_parms("Fix_Rm_Rp_info", Pc_var_list);
}

/***************************************************************************
 * Get_all_printcap_entries( char *s )
 *  - get the remote host and remote printer information
 *  - we assume this is called by clients trying to get remote host
 *    connection information
 *  - we may want to get the printcap information as a side effect
 *
 ***************************************************************************/

void Get_all_printcap_entries(void)
{
	const char *s, *t;
	int i;

	/*
	 * now check to see if we have an entry for the 'all:' printcap
	 */
	s = t = NULL;
	DEBUG1("Get_all_printcap_entries: starting");
	Free_line_list( &All_line_list );
	if( (s = Select_pc_info(ALL,
			&PC_entry_line_list,
			&PC_alias_line_list,
			&PC_names_line_list, &PC_order_line_list,
			&PC_info_line_list, 0, 0 )) ){
		if( !(t = Find_str_value( &PC_entry_line_list, ALL )) ){
			t = "all";
		}
		DEBUG1("Get_all_printcap_entries: '%s' has '%s'",s,t);
		Split(&All_line_list,t,File_sep,0,0,0,1,0,0);
	} else {
		for( i = 0; i < PC_order_line_list.count; ++i ){
			s = PC_order_line_list.list[i];
			if( ISNULL(s) || !safestrcmp( ALL, s ) ) continue;
			if( safestrcmp(s,"*") && !ispunct( cval( s ) ) ){
				Add_line_list(&All_line_list,s,0,0,0);
			}
		}
	}
	if(DEBUGL1)Dump_line_list("Get_all_printcap_entries- All_line_list", &All_line_list );
}

void Show_formatted_info( void )
{
	char *s;
	char error[SMALLBUFFER];

	DEBUG1("Show_formatted_info: getting printcap information for '%s'", Printer_DYN );
	error[0] = 0;
	Fix_Rm_Rp_info(error,sizeof(error));
	if( error[0] ){
		WARNMSG(
			"%s: '%s'",
			Printer_DYN, error );
	}
	if(DEBUGL1)Dump_line_list("Aliases",&PC_alias_line_list);
	s = Join_line_list_with_sep(&PC_alias_line_list,"|");
	if( Write_fd_str( 1, s ) < 0 ) cleanup(0);
	free(s); 
	/* Escape_colons( &PC_entry_line_list ); */
	s = Join_line_list_with_sep(&PC_entry_line_list,"\n :");
	Expand_percent( &s );
	if( s ){
		if( Write_fd_str( 1, "\n :" ) < 0 ) cleanup(0);
		if( Write_fd_str( 1, s ) < 0 ) cleanup(0);
	}
	free(s); s =NULL;
	if( Write_fd_str( 1, "\n" ) < 0 ) cleanup(0);
}

void Show_all_printcap_entries( void )
{
	char *s;
	int i;

	s = 0;
	Get_all_printcap_entries();
	s = Join_line_list_with_sep(&PC_names_line_list,"\n :");
	if( Write_fd_str( 1, "\n.names\n" ) < 0 ) cleanup(0);
	if( s && *s ){
		if( Write_fd_str( 1, " :" ) < 0 ) cleanup(0);
		if( Write_fd_str( 1, s ) < 0 ) cleanup(0);
		if( Write_fd_str( 1, "\n" ) < 0 ) cleanup(0);
	}
	free(s); 

	s = Join_line_list_with_sep(&All_line_list,"\n :");
	if( Write_fd_str( 1, "\n.all\n" ) < 0 ) cleanup(0);
	if( s && *s ){
		if( Write_fd_str( 1, " :" ) < 0 ) cleanup(0);
		if( Write_fd_str( 1, s ) < 0 ) cleanup(0);
		if( Write_fd_str( 1, "\n" ) < 0 ) cleanup(0);
	}
	free(s); s =NULL;

	if( Write_fd_str( 1,"\n#Printcap Information\n") < 0 ) cleanup(0);
	for( i = 0; i < All_line_list.count; ++i ){
		Set_DYN(&Printer_DYN,All_line_list.list[i]);
		Show_formatted_info();
	}
}
