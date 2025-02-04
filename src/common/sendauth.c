/***************************************************************************
 * LPRng - An Extended Print Spooler System
 *
 * Copyright 1988-2003, Patrick Powell, San Diego, CA
 *     papowell@lprng.com
 * See LICENSE for conditions of use.
 *
 ***************************************************************************/

#include "lp.h"
#include "user_auth.h"
#include "sendjob.h"
#include "permission.h"
#include "getqueue.h"
#include "errorcodes.h"
#include "linksupport.h"
#include "krb5_auth.h"
#include "fileopen.h"
#include "child.h"
#include "gethostinfo.h"
#include "sendauth.h"
/**** ENDINCLUDE ****/

/***************************************************************************
 * Commentary:
 * Patrick Powell Mon Apr 17 05:43:48 PDT 1995
 * 
 * The protocol used to send a secure job consists of the following
 * following:
 * 
 * Client                                   Server
 * \REQ_SECUREprintername C/F user\n - receive a command
 *             0           1   2
 * \REQ_SECUREprintername C/F user controlfile\n - receive a job
 *             0           1   2
 *          
 * 1. Get a temporary file
 * 2. Generate the compressed data files - this has the format
 *      Authentication
 *      \n
 *      \3count cfname\n
 *      [count control file bytes]
 *      \4count dfname\n
 *      [count data file bytes]
 *
 * 3. send the \REQ_SECRemotePrinter_DYN user@RemoteHost_DYN file size\n
 *    string to the remote RemoteHost_DYN, wait for an ACK
 *
 * 4. send the compressed data files - this has the format
 *      wait for an ACK
 ***************************************************************************/

static void Put_in_auth( int tempfd, const char *key, char *value );

/*
 * Send_auth_transfer
 *  1. we send the command line and wait for ACK of 0
 *  \REQ_SEQUREprinter C/F sender_id authtype [jobsize]
 *  2. if authtype == kerberos we do kerberos
 *      - send a file to the remote end
 *      - get back a file
 *  3. if authtype == pgp we do pgp
 *      - same as kerberos
 *  3. if otherwise,  we start a process with command line options
 *       fd 0 -  sock
 *       fd 1 -  for reports
 *       fd 2 -  for errors
 *    /filter -C -P printer -n sender_id -A authtype -R remote_id -Ttempfile
 *    The tempfile will be sent to the remote end and status
 *     written back on fd 2
 *     - we save this information
 *     - reopen the file and put error messages in it.
 *  RETURN:
 *     0 - no error
 *     !=0 - error
 */

int Send_auth_transfer( int *sock, int transfer_timeout,
	struct job *job, struct job *logjob, char *error, int errlen, char *cmd,
	const struct security *security, struct line_list *info )
{
	struct stat statb;
	int ack, len, n, fd;		/* ACME! The best... */
	int status = JFAIL;			/* job status */
	char *secure, *destination, *from, *client, *s;
	char *tempfile;
	char buffer[SMALLBUFFER];
	errno = 0;

	secure = 0;
	fd = Make_temp_fd(&tempfile);

	if( cmd && (s = safestrrchr(cmd,'\n')) ) *s = 0;
	DEBUG1("Send_auth_transfer: cmd '%s'", cmd );

	if(DEBUGL1)Dump_line_list("Send_auth_transfer: info ", info );

	destination = Find_str_value(info, DESTINATION );
	from = Find_str_value(info, FROM );
	client = Find_str_value(info, CLIENT );

	if( safestrcmp(security->config_tag, "kerberos") ){
		Put_in_auth(fd,DESTINATION,destination);
		if( Is_server ) Put_in_auth(fd,SERVER,from);
		Put_in_auth(fd,CLIENT,client);
		if( cmd ){
			Put_in_auth(fd,INPUT,cmd);
		}
	} else {
		if( cmd && (Write_fd_str(fd,cmd) < 0 || Write_fd_str(fd,"\n") < 0) ){
			plp_snprintf(error, errlen, "Send_auth_transfer: '%s' write failed - %s",
				tempfile, Errormsg(errno) );
			goto error;
		}
		if( Is_server && (Write_fd_str(fd,client) < 0 || Write_fd_str(fd,"\n") < 0) ){
			plp_snprintf(error, errlen, "Send_auth_transfer: '%s' write failed - %s",
				tempfile, Errormsg(errno) );
			goto error;
		}
	}

	if( Write_fd_str(fd,"\n") < 0 ){
		plp_snprintf(error, errlen, "Send_auth_transfer: '%s' write failed - %s",
			tempfile, Errormsg(errno) );
		goto error;
	}

	s = Find_str_value(info, CMD );
	if( job ){
        status = Send_normal( &fd, job, logjob, transfer_timeout, fd, 0);
        if( status ) return( status );
		errno = 0;
		if( stat(tempfile,&statb) ){
			Errorcode = JABORT;
			logerr_die(LOG_INFO, "Send_auth_transfer: stat '%s' failed",
				tempfile);
		}
		plp_snprintf( buffer,sizeof(buffer), " %0.0f",(double)(statb.st_size) );
		secure = safestrdup3(s,buffer,"\n",__FILE__,__LINE__);
	} else {
		secure = safestrdup2(s,"\n",__FILE__,__LINE__);
	}
	close( fd ); fd = -1;

	/* send the message */
	DEBUG3("Send_auth_transfer: sending '%s'", secure );
	status = Link_send( RemoteHost_DYN, sock, transfer_timeout,
		secure, safestrlen(secure), &ack );
	DEBUG3("Send_auth_transfer: status '%s'", Link_err_str(status) );
	if( status ){
		/* open output file */
		if( (fd = Checkwrite(tempfile,&statb,O_WRONLY|O_TRUNC,1,0)) < 0){
			Errorcode = JABORT;
			logerr_die(LOG_INFO, "Send_auth_transfer: open '%s' for write failed",
				tempfile);
		}
		/* we turn off IO from the socket */
		shutdown(*sock,1);
		if( (s = safestrchr(secure,'\n')) ) *s = 0;
		plp_snprintf( error, errlen,
			"error '%s' sending '%s' to %s@%s\n",
			Link_err_str(status), secure, RemotePrinter_DYN, RemoteHost_DYN );
		Write_fd_str( fd, error );
		error[0] = 0;
		DEBUG2("Send_auth_transfer: starting read");
		len = 0;
		while( (n = Read_fd_len_timeout(Send_query_rw_timeout_DYN, *sock,buffer+len,sizeof(buffer)-1-len)) > 0 ){
			buffer[n+len] = 0;
			DEBUG4("Send_auth_transfer: read '%s'", buffer);
			while( (s = strchr(buffer,'\n')) ){
				*s++ = 0;
				DEBUG2("Send_auth_transfer: doing '%s'", buffer);
				plp_snprintf(error,errlen, "%s\n", buffer );
				if( Write_fd_str(fd,error) < 0 ){
					Errorcode = JABORT;
					logerr(LOG_INFO, "Send_auth_transfer: write '%s' failed",
						tempfile );
					goto error;
				}
				memmove(buffer,s,safestrlen(s)+1);
			}
			len = safestrlen(buffer);
		}
		if( buffer[0] ){
			DEBUG2("Send_auth_transfer: doing '%s'", buffer);
			plp_snprintf(error,errlen, "%s\n", buffer );
			if( Write_fd_str(fd,error) < 0 ){
				Errorcode = JABORT;
				logerr(LOG_INFO, "Send_auth_transfer: write '%s' failed",
					tempfile );
				goto error;
			}
		}

		close( fd ); fd = -1;
		error[0] = 0;
		goto error;
	}

	/*
     * now we do the protocol dependent exchange
     */

	status = security->client_send( sock, transfer_timeout, tempfile,
		error, errlen, security, info );

 error:

	DEBUG3("Send_auth_transfer: sock %d, exit status %d, error '%s'",
		*sock, status, error );
	/* we are going to put the returned error status in the temp file
	 * as the device to read from
	 */
	free(secure); secure = NULL;
	if( error[0] ){
		if( job ){
			setstatus(logjob, "Send_auth_transfer: %s", error );
			Set_str_value(&job->info,ERROR,error);
			Set_nz_flag_value(&job->info,ERROR_TIME,time(0));
		}
		if( (fd = Checkwrite(tempfile,&statb,O_WRONLY|O_TRUNC,1,0)) < 0){
			Errorcode = JFAIL;
			logerr_die(LOG_INFO, "Send_auth_transfer: cannot open '%s'", tempfile );
		}
		Write_fd_str(fd,error);
		close( fd ); fd = -1;
		error[0] = 0;
	}
	if( *sock >= 0 ){
		if( (fd = Checkread(tempfile,&statb)) < 0 ){
			Errorcode = JFAIL;
			logerr_die(LOG_INFO, "Send_auth_transfer: cannot open '%s'", tempfile );
		}
		if( dup2( fd, *sock ) == -1 ){
			Errorcode = JFAIL;
			logerr_die(LOG_INFO, "Send_auth_transfer: dup2(%d,%d)", fd, *sock );
		}
		if( fd != *sock ) close(fd);
		fd = -1;
	}
	Free_line_list(info);
	DEBUG3("Send_auth_transfer: exit status %d, error '%s'",
		status, error );
	return( status );
}

/***************************************************************************
 *
 * struct security *Fix_send_auth( char *name, struct line_list *info
 * 	char *error, int errlen )
 * 
 * Find the information about the encrypt type and then make up the string
 * to send to the server requesting the encryption
 **************************************************************************/

const struct security *Fix_send_auth( char *name, struct line_list *info,
	struct job *job, char *error, int errlen )
{
	const struct security *security = 0;
	char buffer[SMALLBUFFER], *from, *client, *destination;
	const char *tag, *server_tag, *key;

	if( name == 0 ){
		if( Is_server ){
			name = Auth_forward_DYN;
		} else {
			name = Auth_DYN;
		}
	}
	DEBUG1("Fix_send_auth: name '%s'", name );
	if( name ){
		security = FindSecurity(name);
		if( !security ){
			plp_snprintf(error, errlen,
				"Fix_send_auth: '%s' security not supported", name );
			goto error;
		} else {
			DEBUG1("Fix_send_auth: name '%s' matches '%s'", name, security->name );
		}
	} else {
		DEBUG1("Fix_send_auth: no security" );
		return( 0 );
	}

	/* check to see if we use unix_socket */
	if( security->auth_flags & IP_SOCKET_ONLY ){
		Set_DYN( &Unix_socket_path_DYN, 0 );
	}

	if( !(tag = security->config_tag) ) tag = security->name;
	plp_snprintf(buffer,sizeof(buffer), "%s_", tag );
	Find_default_tags( info, Pc_var_list, buffer );
	Find_tags( info, &Config_line_list, buffer );
	Find_tags( info, &PC_entry_line_list, buffer );
	Expand_hash_values( info );
	if(DEBUGL1)Dump_line_list("Fix_send_auth: found info", info );

	if( !(tag = security->config_tag) ) tag = security->name;
	if( !(server_tag = security->server_tag) ) server_tag = tag;
	if( Is_server ){
		/* forwarding */
		key = "F";
		from = Find_str_value(info,ID);
		if(!from)from = Find_str_value(info,"server_principal");
		if( from == 0 && safestrcmp(tag,"kerberos") && safestrcmp(tag,"none") ){
			plp_snprintf(error, errlen,
			"Fix_send_auth: '%s' security missing '%s_id' info", tag, tag );
			goto error;
		}
		Set_str_value(info,FROM,from);
		if( job ){
			client = Find_str_value(&job->info,AUTHUSER);
			Set_str_value(info,CLIENT,client);
		} else {
			client = (char *)Perm_check.authuser;
		}
		if( client == 0 
			&& !(client = Find_str_value(info,"default_client_name"))
			&& safestrcmp(tag,"none") ){
			plp_snprintf(error, errlen,
			"Fix_send_auth: security '%s' missing authenticated client", tag );
			goto error;
		}
		Set_str_value(info,CLIENT,client);
		destination = Find_str_value(info,FORWARD_ID);
		if(!destination)destination = Find_str_value(info,"forward_principal");
		if( destination == 0 && safestrcmp(tag, "kerberos")
			&& safestrcmp(tag, "none")){
			plp_snprintf(error, errlen,
			"Fix_send_auth: '%s' security missing '%s_forward_id' info", tag, tag );
			goto error;
		}
	} else {
		/* from client */
		key = "C";
		from = Logname_DYN;
		Set_str_value(info,FROM,from);
		client = Logname_DYN;
		Set_str_value(info,CLIENT,client);
		destination = Find_str_value(info,ID);
		if(!destination)destination = Find_str_value(info,"server_principal");
		if( destination == 0 && safestrcmp(tag, "kerberos")
			&& safestrcmp(tag, "none") ){
			plp_snprintf(error, errlen,
			"Fix_send_auth: '%s' security missing destination '%s_id' info", tag, tag );
			goto error;
		}
	}

	Set_str_value(info,DESTINATION,destination);

	DEBUG1("Fix_send_auth: pr '%s', key '%s', from '%s', name '%s', tag '%s'",
		RemotePrinter_DYN,key, from, server_tag, tag);
	plp_snprintf( buffer, sizeof(buffer),
		"%c%s %s %s %s",
		REQ_SECURE,RemotePrinter_DYN,key, from, server_tag );
	Set_str_value(info,CMD,buffer);
	DEBUG1("Fix_send_auth: sending '%s'", buffer );

 error:
	if( error[0] ) security = 0;
	DEBUG1("Fix_send_auth: error '%s'", error );
	if(DEBUGL1)Dump_line_list("Fix_send_auth: info", info );
  
	return(security);
}

void Put_in_auth( int tempfd, const char *key, char *value )
{
	char *v = Escape(value,1);
	DEBUG1("Put_in_auth: fd %d, key '%s' value '%s', v '%s'",
		tempfd, key, value, v );
	if(
		Write_fd_str(tempfd,key) < 0
		|| Write_fd_str(tempfd,"=") < 0
		|| Write_fd_str(tempfd,v) < 0
		|| Write_fd_str(tempfd,"\n") < 0
		){
		Errorcode = JFAIL;
		logerr_die(LOG_INFO, "Put_in_auth: cannot write to file" );
	}
	free(v); 
}
