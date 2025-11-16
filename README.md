## Instalando Dependências do Projeto
1. Instale a ESP-IDF

Pela própria extensão do VS Code baixe a ESP-IDF, a versâo testada pela equipe foi a v5.5.1.

2. Instale localmente o Bluepad32

Efetue os seguintes comandos na raiz do projeto
```sh
   cd ./external/btstack/port/esp32
   $env:IDF_PATH = "../../../../"
   python ./integrate_btstack.py
```



## License

- Example code: licensed under Public Domain.
- Bluepad32: licensed under Apache 2.
- BTstack:
    - Free to use for open source projects.
    - Paid for commercial projects.
    - <https://github.com/bluekitchen/btstack/blob/master/LICENSE>
