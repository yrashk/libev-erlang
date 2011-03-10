-module(nifnet_app).
-behaviour(application).

-export([start/2, stop/1]).

start(_StartType, _StartArgs) ->
    ok.

stop(_State) ->
    ok.

