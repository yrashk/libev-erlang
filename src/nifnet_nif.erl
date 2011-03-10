-module(nifnet_nif).

-export([start/0,connect/3, send/2, recv/2, stop/1, listen/3,
         accept/1, close/1]).

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
    "NIF library not loaded".

stop(_Ref) ->
    "NIF library not loaded".

connect(_Ctx, _Ip, _Host) ->
    "NIF library not loaded".

send(_Sock, _Msg) ->
    "NIF library not loaded".

recv(_Sock, _Size) ->
    "NIF library not loaded".

listen(_Ctx, _Port, _Backlog) ->
    "NIF library not loaded".

accept(_Sock) ->
    "NIF library not loaded".

close(_Sock) ->
    "NIF library not loaded".

%% ===================================================================
%% EUnit tests
%% ===================================================================
-ifdef(TEST).

basic_test() ->
    {ok, Ref} = start(),
    ok = stop(Ref).

-endif.

