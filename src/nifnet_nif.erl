-module(nifnet_nif).

-export([start/0,connect/3, send/2, recv/3, stop/1, listen/3,
         accept/2, shutdown/2, close/1]).

-on_load(init/0).

-ifdef(TEST).
-include_lib("eunit/include/eunit.hrl").
-endif.

init() ->
    case code:which(nifnet_nif) of
        Filename when is_list(Filename) ->
            erlang:load_nif(filename:join([filename:dirname(Filename),"../priv/nifnet_drv"]), []);
        Err ->
            Err
    end.

start() ->
    erlang:nif_error(not_loaded).

stop(_Ref) ->
    erlang:nif_error(not_loaded).

connect(_Ctx, _Ip, _Host) ->
    erlang:nif_error(not_loaded).

send(_Sock, _Msg) ->
    erlang:nif_error(not_loaded).

recv(_Sock, _Size, _Timeout) ->
    erlang:nif_error(not_loaded).

listen(_Ctx, _Port, _Backlog) ->
    erlang:nif_error(not_loaded).

accept(_Sock, _Timeout) ->
    erlang:nif_error(not_loaded).

shutdown(_Sock, _How) ->
    erlang:nif_error(not_loaded).

close(_Sock) ->
    erlang:nif_error(not_loaded).

%% ===================================================================
%% EUnit tests
%% ===================================================================
-ifdef(TEST).

basic_test() ->
    {ok, Ref} = start(),
    ok = stop(Ref).

-endif.

