# Dotfiles

  $ ls -a ~
  .  ..  .bashrc  .config/  .gitconfig  .vimrc  .zshrc  42/  Documents/

  > dotfiles: config files whose name starts with ".", hidden from a
    plain ls
  > your whole setup = a handful of text files
  > keep them in git, link them into place with GNU stow, share them
    online

# new machine, same rice

  $ git clone <your-repo> ~/dotfiles
  dotfiles/
  ├── zsh/.zshrc
  ├── nvim/.config/nvim/init.lua
  └── kitty/.config/kitty/kitty.conf

  $ cd ~/dotfiles && stow zsh nvim kitty
  # symlinks each one into ~ -> done
  # the only repo you'll commit to every week for the next 10 years

  On UC.OS your dotfile is ~/.shrc: cat it, every line is explained.

  # UNIX_CLUB: planned: a shared repo of members' dotfiles + a gallery
  # of setups . a themed setup showcase / contest (to be confirmed) .
  # an Intra achievement for the winner (aspirational)
