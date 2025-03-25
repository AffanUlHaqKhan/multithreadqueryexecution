This repo contains sample code for runnig query in multi threaded mode
There are multiple type of sample codes provided to run queries in multi threaded environment
This code is using NEO4J as the graph databse for now

NEO4J queries can be entered as a json element. The queries are in cypher query language, which is default for NEO4J

-> Execute single query in multi thred by breaking it up in multiple parts
-> Execute multiple query, each running its own thread
-> Execute multiple queries in their own threads, each big query invloving operation like union etc, are divided into smaller queries and then the result is combined to get a single output
