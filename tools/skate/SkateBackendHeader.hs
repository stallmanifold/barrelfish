{- 
  SkateBackendHeader: Backend for generating C header files
   
  Part of Skate: a Schema specification languge
   
  Copyright (c) 2017, ETH Zurich.
  All rights reserved.
  
  This file is distributed under the terms in the attached LICENSE file.
  If you do not find this file, copies can be found by writing to:
  ETH Zurich D-INFK, Universit\"atstr. 6, CH-8092 Zurich. Attn: Systems Group.
-} 

module SkateBackendHeader where

import Data.List
import Data.Char

import qualified CAbsSyntax as C
import SkateParser


import SkateBackendCommon


compile :: String -> String -> SkateParser.Schema -> String
compile infile outfile schema = 
    ""





