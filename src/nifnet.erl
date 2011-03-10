-module(nifnet).
-export([start/0, stop/1, connect/3, send/2, recv/2, listen/2, listen/3,
         accept/1, accept/2, close/1]).

start() ->
    nifnet_nif:start().

stop(Ctx) ->
    nifnet_nif:stop(Ctx).

connect(Ctx, Host, Port) ->
    case inet:getaddr(Host, inet) of
        {ok, Ip} ->
            nifnet_nif:connect(Ctx, Ip, Port);
        What ->
            What
    end.

send(Sock, Msg) ->
    case nifnet_nif:send(Sock, Msg) of
        deferred ->
            receive
                deferred_ok ->
                    ok;
                {deferred_error, Error} ->
                    {error, Error}
            end;
        What ->
            What
    end.

recv(Sock, Size) ->
    case nifnet_nif:recv(Sock, Size) of
        deferred ->
            receive
                {deferred_ok, Data} ->
                    {ok, Data};
                {deferred_error, Error} ->
                    {error, Error}
            end;
        What ->
            What
    end.

listen(Ctx, Port) ->
    nifnet_nif:listen(Ctx, Port, 5).
listen(Ctx, Port, Backlog) ->
    nifnet_nif:listen(Ctx, Port, Backlog).

accept(LSock) ->
    accept(LSock, infinity).

accept(LSock, Timeout) ->
    case nifnet_nif:accept(LSock, Timeout) of
        deferred ->
            receive
                {deferred_ok, Sock} ->
                    {ok, Sock};
                {deferred_error, Error} ->
                    {error, Error}
            end;
        What ->
            What
    end.

close(Sock) ->
    nifnet_nif:close(Sock).

