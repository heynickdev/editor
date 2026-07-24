make:
  gcc -std=c23 -Wall -Wextra -Wpedantic editor.c -o editor

clean:
  rm -rf editor; gcc -std=c23 -Wall -Wextra -Wpedantic editor.c -o editor
