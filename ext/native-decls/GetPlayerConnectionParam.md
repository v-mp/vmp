---
ns: CFX
apiset: server
---
## GET_PLAYER_CONNECTION_PARAM

```c
char* GET_PLAYER_CONNECTION_PARAM(char* playerSrc, char* paramName);
```

Returns the value of a query parameter passed in the connect URL the player used to join
(e.g. `vmp://connect/host:30120?token=ABC` makes `GetPlayerConnectionParam(src, 'token')` return `ABC`).

Can be called during the `playerConnecting` event.

## Parameters
* **playerSrc**: The player to get the connection parameter for.
* **paramName**: The name of the query parameter (e.g. `token`).

## Return value
The parameter value, or `nil` if the player did not pass it.
