# Bateria sem fio

Mostra na bandeja do Windows a porcentagem de bateria de cada dispositivo Bluetooth compatível. A leitura acontece ao abrir e se repete uma vez por minuto.

## Como usar

Baixe o arquivo `BateriaSemFio.zip` na página da versão mais recente. [Releases](https://github.com/FelipeGS99/bateria-sem-fio/releases/latest)

1. Extraia o ZIP. Abra `BateriaSemFio.exe` no Windows 10 ou 11. Em um computador com Windows em ARM, use `BateriaSemFio-arm64.exe`.
2. Conecte seus dispositivos Bluetooth ao PC. O número no ícone é a porcentagem. Passe o mouse para ver o nome ou clique em qualquer ícone para ver a lista e usar **Atualizar agora** ou **Sair**.
3. Se os ícones aparecerem sob a seta **^**, arraste-os para perto do relógio ou habilite-os nas configurações da barra de tarefas.

O aplicativo é portátil e roda localmente, sem instalação, acesso à internet ou permissões de administrador.

## Compatibilidade

O aplicativo consulta as porcentagens que os drivers Bluetooth informam ao Windows. A indicação **Windows** na dica do ícone identifica esse valor, que pode corresponder à última leitura registrada pelo sistema. Para dispositivos Bluetooth LE sem esse valor, o aplicativo tenta ler a característica **Battery Level** do serviço padrão **Battery Service**. Se a leitura não estiver disponível, o nível não aparece. Se várias leituras da mesma fonte forem agrupadas para um único dispositivo, o ícone exibe a menor.

Receptores USB proprietários de 2,4 GHz, estojos e níveis separados de fones dependem das informações que o modelo expõe. Consulte a documentação do fabricante para saber quais níveis o modelo informa ao Windows.

## Código-fonte

Na pasta `codigo-fonte`, `app.cpp` implementa a bandeja e `battery.cpp` lê os níveis usando as APIs do Windows.

Para recompilar, abra a pasta `codigo-fonte` e use Zig 0.15.2. No Linux com terminal POSIX, execute `./build.sh` para x64 ou `./build.sh aarch64-windows-gnu BateriaSemFio-arm64.exe` para ARM64. No Windows, rode os dois comandos abaixo no PowerShell, na pasta do código.

```powershell
zig rc /fo app.res app.rc
zig c++ -target x86_64-windows-gnu -std=c++20 -Os -DNDEBUG -DUNICODE -D_UNICODE -static -s -municode '-Wl,--subsystem,windows' app.cpp battery.cpp app.res -o BateriaSemFio.exe -lsetupapi -lcfgmgr32 -lbluetoothapis -lole32 -lshell32 -luser32 -lgdi32
```
