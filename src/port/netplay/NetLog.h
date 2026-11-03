#ifndef NETPLAY_NET_LOG_H
#define NETPLAY_NET_LOG_H

/**
 * Writes a "[Netplay] ..." line, printf-style, to stdout (which the test scripts read) and to the game's log file,
 * where it survives a game launched from Finder.
 */
void NetLog(const char* format, ...);

#endif // NETPLAY_NET_LOG_H
